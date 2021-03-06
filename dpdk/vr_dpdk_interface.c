/*
 * Copyright (C) 2014 Semihalf.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * vr_dpdk_interface.c -- vRouter interface callbacks
 *
 */

#include "vr_dpdk.h"
#include "vr_dpdk_netlink.h"
#include "vr_dpdk_usocket.h"
#include "vr_dpdk_virtio.h"

#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_ip.h>

/*
 * dpdk_virtual_if_add - add a virtual (virtio) interface to vrouter.
 * Returns 0 on success, < 0 otherwise.
 */
static int
dpdk_virtual_if_add(struct vr_interface *vif)
{
    int ret;
    unsigned int nrxqs, ntxqs;

    RTE_LOG(INFO, VROUTER, "Adding vif %u virtual device %s\n",
                vif->vif_idx, vif->vif_name);

    nrxqs = vr_dpdk_virtio_nrxqs(vif);
    ntxqs = vr_dpdk_virtio_ntxqs(vif);

    ret = vr_dpdk_lcore_if_schedule(vif, vr_dpdk_lcore_least_used_get(),
               nrxqs, &vr_dpdk_virtio_rx_queue_init,
               ntxqs, &vr_dpdk_virtio_tx_queue_init);
    if (ret) {
        return ret;
    }

    /*
     * When something goes wrong, vr_netlink_uvhost_vif_add() returns
     * non-zero value. Then we return this value here. It is handled by
     * dp-core and dpdk_virtual_if_del() is called, so there is no need
     * to do it manually here.
     *
     * Check dp-core/vf_interface.c:eth_drv_add() for reference.
     */
    return vr_netlink_uvhost_vif_add(vif->vif_name, vif->vif_idx,
                                    nrxqs, ntxqs);
}

/*
 * dpdk_virtual_if_del - deletes a virtual (virtio) interface from vrouter.
 * Returns 0 on success, -1 otherwise.
 */
static int
dpdk_virtual_if_del(struct vr_interface *vif)
{
    int ret;

    RTE_LOG(INFO, VROUTER, "Deleting vif %u virtual device\n",
                vif->vif_idx);

    ret = vr_netlink_uvhost_vif_del(vif->vif_idx);

    vr_dpdk_lcore_if_unschedule(vif);

    /*
     * TODO - User space vhost thread need to ack the deletion of the vif.
     */

    return ret;
}

static inline void
dpdk_dbdf_to_pci(unsigned int dbdf,
        struct rte_pci_addr *address)
{
    address->domain = (dbdf >> 16);
    address->bus = (dbdf >> 8) & 0xff;
    address->devid = (dbdf & 0xf8);
    address->function = (dbdf & 0x7);

    return;
}

static inline unsigned
dpdk_pci_to_dbdf(struct rte_pci_addr *address)
{
    return address->domain << 16
        | address->bus << 8
        | address->devid
        | address->function;
}

/* mirrors the function used in bonding */
static inline uint8_t
dpdk_find_port_id_by_pci_addr(struct rte_pci_addr *addr)
{
    uint8_t i;
    struct rte_pci_addr *eth_pci_addr;

    for (i = 0; i < rte_eth_dev_count(); i++) {
        if (rte_eth_devices[i].pci_dev == NULL)
            continue;

        eth_pci_addr = &(rte_eth_devices[i].pci_dev->addr);
        if (addr->bus == eth_pci_addr->bus &&
            addr->devid == eth_pci_addr->devid &&
            addr->domain == eth_pci_addr->domain &&
            addr->function == eth_pci_addr->function) {
            return i;
        }
    }

    return VR_DPDK_INVALID_PORT_ID;
}
static inline void
dpdk_find_pci_addr_by_port(struct rte_pci_addr *addr, uint8_t port_id)
{
    rte_memcpy(addr, &rte_eth_devices[port_id].pci_dev->addr, sizeof(struct rte_pci_addr));
}

int
dpdk_vif_attach_ethdev(struct vr_interface *vif,
        struct vr_dpdk_ethdev *ethdev)
{
    int ret = 0;
    struct ether_addr mac_addr;
    struct rte_eth_dev_info dev_info;

    vif->vif_os = (void *)ethdev;

    rte_eth_dev_info_get(ethdev->ethdev_port_id, &dev_info);
    if (dev_info.tx_offload_capa & DEV_TX_OFFLOAD_IPV4_CKSUM) {
        vif->vif_flags |= VIF_FLAG_TX_CSUM_OFFLOAD;
    } else {
        vif->vif_flags &= ~VIF_FLAG_TX_CSUM_OFFLOAD;
    }

    memset(&mac_addr, 0, sizeof(mac_addr));
    /*
     * do not want to overwrite what agent had sent. set only if
     * the address has null
     */
    if (!memcmp(vif->vif_mac, mac_addr.addr_bytes, ETHER_ADDR_LEN)) {
        rte_eth_macaddr_get(ethdev->ethdev_port_id, &mac_addr);
        memcpy(vif->vif_mac, mac_addr.addr_bytes, ETHER_ADDR_LEN);
    }

    return ret;
}

/* Add fabric interface */
static int
dpdk_fabric_if_add(struct vr_interface *vif)
{
    int ret;
    uint8_t port_id;
    struct rte_pci_addr pci_address;
    struct vr_dpdk_ethdev *ethdev;
    struct ether_addr mac_addr;

    memset(&pci_address, 0, sizeof(pci_address));
    if (vif->vif_flags & VIF_FLAG_PMD) {
        if (vif->vif_os_idx >= rte_eth_dev_count()) {
            RTE_LOG(ERR, VROUTER, "Invalid PMD device index %u"
                    " (must be less than %u)\n",
                    vif->vif_os_idx, (unsigned)rte_eth_dev_count());
            return -ENOENT;
        }

        port_id = vif->vif_os_idx;
        /* TODO: does not work for host interfaces
        dpdk_find_pci_addr_by_port(&pci_address, port_id);
        vif->vif_os_idx = dpdk_pci_to_dbdf(&pci_address);
        */
    } else {
        dpdk_dbdf_to_pci(vif->vif_os_idx, &pci_address);
        port_id = dpdk_find_port_id_by_pci_addr(&pci_address);
        if (port_id == VR_DPDK_INVALID_PORT_ID) {
            RTE_LOG(ERR, VROUTER, "Error adding vif %u eth device %s:"
                " no port ID found for PCI " PCI_PRI_FMT "\n",
                    vif->vif_idx, vif->vif_name,
                    pci_address.domain, pci_address.bus,
                    pci_address.devid, pci_address.function);
            return -ENOENT;
        }
    }

    memset(&mac_addr, 0, sizeof(mac_addr));
    rte_eth_macaddr_get(port_id, &mac_addr);

    RTE_LOG(INFO, VROUTER, "Adding vif %u eth device %" PRIu8 " PCI " PCI_PRI_FMT
        " MAC " MAC_FORMAT "\n",
        vif->vif_idx, port_id, pci_address.domain, pci_address.bus,
        pci_address.devid, pci_address.function,
        MAC_VALUE(mac_addr.addr_bytes));

    ethdev = &vr_dpdk.ethdevs[port_id];
    if (ethdev->ethdev_ptr != NULL) {
        RTE_LOG(ERR, VROUTER, "    error adding eth dev %s: already added\n",
                vif->vif_name);
        return -EEXIST;
    }
    ethdev->ethdev_port_id = port_id;

    /* init eth device */
    ret = vr_dpdk_ethdev_init(ethdev);
    if (ret != 0)
        return ret;

    ret = dpdk_vif_attach_ethdev(vif, ethdev);
    if (ret)
        return ret;

    ret = rte_eth_dev_start(port_id);
    if (ret < 0) {
        RTE_LOG(ERR, VROUTER, "    error starting eth device %" PRIu8
                ": %s (%d)\n", port_id, rte_strerror(-ret), -ret);
        return ret;
    }

    ret = vr_dpdk_ethdev_rss_init(ethdev);
    if (ret < 0)
        return ret;

    /* we need to init Flow Director after the device has started */
#if VR_DPDK_USE_HW_FILTERING
    /* init hardware filtering */
    ret = vr_dpdk_ethdev_filtering_init(vif, ethdev);
    if (ret < 0)
        return ret;
#endif

    /* schedule RX/TX queues */
    return vr_dpdk_lcore_if_schedule(vif, vr_dpdk_lcore_least_used_get(),
        ethdev->ethdev_nb_rss_queues, &vr_dpdk_ethdev_rx_queue_init,
        ethdev->ethdev_nb_tx_queues, &vr_dpdk_ethdev_tx_queue_init);
}

/* Delete fabric interface */
static int
dpdk_fabric_if_del(struct vr_interface *vif)
{
    uint8_t port_id;

    RTE_LOG(INFO, VROUTER, "Deleting vif %u\n", vif->vif_idx);

    /*
     * If dpdk_fabric_if_add() failed before dpdk_vif_attach_ethdev,
     * then vif->vif_os will be NULL.
     */
    if (vif->vif_os == NULL) {
        RTE_LOG(ERR, VROUTER, "    error deleting eth dev %s: already removed\n",
                vif->vif_name);
        return -EEXIST;
    }

    port_id = (((struct vr_dpdk_ethdev *)(vif->vif_os))->ethdev_port_id);

    /* unschedule RX/TX queues */
    vr_dpdk_lcore_if_unschedule(vif);

    rte_eth_dev_stop(port_id);

    /* release eth device */
    return vr_dpdk_ethdev_release(vif->vif_os);
}

/* Add vhost interface */
static int
dpdk_vhost_if_add(struct vr_interface *vif)
{
    uint8_t port_id, slave_port_id = VR_DPDK_INVALID_PORT_ID;
    int ret;
    struct ether_addr mac_addr;
    struct vr_dpdk_ethdev *ethdev;

    if (vif->vif_flags & VIF_FLAG_PMD) {
        port_id = vif->vif_os_idx;
    }
    else {
        /* The Agent passes xconnect fabric interface in cross_connect_idx,
         * but dp-core does not copy it into vr_interface. Instead
         * it looks for an interface with os_idx == cross_connect_idx
         * and sets vif->vif_bridge if there is such an interface.
         */
        ethdev = (struct vr_dpdk_ethdev *)(vif->vif_bridge->vif_os);
        if (ethdev == NULL) {
            RTE_LOG(ERR, VROUTER, "Error adding vif %u KNI device %s:"
                " bridge vif %u ethdev is not initialized\n",
                    vif->vif_idx, vif->vif_name, vif->vif_bridge->vif_idx);
            return -ENOENT;
        }
        port_id = ethdev->ethdev_port_id;
        /*
         * KNI does not support bond interfaces and generate random MACs,
         * so we try to get a bond member instead.
         */
        if (ethdev->ethdev_nb_slaves > 0)
            slave_port_id = ethdev->ethdev_slaves[0];
    }

    /* get interface MAC address */
    memset(&mac_addr, 0, sizeof(mac_addr));
    rte_eth_macaddr_get(port_id, &mac_addr);

    RTE_LOG(INFO, VROUTER, "Adding vif %u KNI device %s at eth device %" PRIu8
                " MAC " MAC_FORMAT "\n",
                vif->vif_idx, vif->vif_name, port_id, MAC_VALUE(mac_addr.addr_bytes));

    if (slave_port_id != VR_DPDK_INVALID_PORT_ID) {
        port_id = slave_port_id;

        memset(&mac_addr, 0, sizeof(mac_addr));
        rte_eth_macaddr_get(port_id, &mac_addr);
        RTE_LOG(INFO, VROUTER, "    using bond slave eth device %" PRIu8
                " MAC " MAC_FORMAT "\n",
                port_id, MAC_VALUE(mac_addr.addr_bytes));
    }

    /* check if KNI is already added */
    if (vr_dpdk.knis[vif->vif_idx] != NULL) {
        RTE_LOG(ERR, VROUTER, "    error adding KNI device %s: already exist\n",
                vif->vif_name);
        return -EEXIST;
    }

    /* init KNI */
    ret = vr_dpdk_knidev_init(port_id, vif);
    if (ret != 0)
        return ret;

    /* add interface to the table of KNIs */
    vr_dpdk.knis[vif->vif_idx] = vif->vif_os;

    /* add interface to the table of vHosts */
    vr_dpdk.vhosts[vif->vif_idx] = vrouter_get_interface(vif->vif_rid, vif->vif_idx);

    return vr_dpdk_lcore_if_schedule(vif, vr_dpdk_lcore_least_used_get(),
            1, &vr_dpdk_kni_rx_queue_init,
            1, &vr_dpdk_kni_tx_queue_init);
}

/* Delete vhost interface */
static int
dpdk_vhost_if_del(struct vr_interface *vif)
{
    RTE_LOG(INFO, VROUTER, "Deleting vif %u KNI device %s\n",
                vif->vif_idx, vif->vif_name);

    /* check if KNI exists */
    if (vr_dpdk.knis[vif->vif_idx] == NULL) {
        RTE_LOG(ERR, VROUTER, "    error deleting KNI device %u: "
                    "device does not exist\n", vif->vif_idx);
        return -EEXIST;
    }

    vr_dpdk_lcore_if_unschedule(vif);

    /* del the interface from the table of vHosts */
    vr_dpdk.vhosts[vif->vif_idx] = NULL;

    /* del the interface from the table of KNIs */
    vr_dpdk.knis[vif->vif_idx] = NULL;

    /* release KNI */
    return vr_dpdk_knidev_release(vif);
}

/* Start interface monitoring */
static void
dpdk_monitoring_start(struct vr_interface *monitored_vif,
    struct vr_interface *monitoring_vif)
{
    uint8_t port_id;

    /* set monitoring redirection */
    vr_dpdk.monitorings[monitored_vif->vif_idx] = monitoring_vif->vif_idx;

    /* set vif flag */
    rte_wmb();
    monitored_vif->vif_flags |= VIF_FLAG_MONITORED;

    if(vif_is_fabric(monitored_vif)) {
        port_id = (((struct vr_dpdk_ethdev *)(monitored_vif->vif_os))->ethdev_port_id);
        rte_eth_promiscuous_enable(port_id);
    }
}

/* Stop interface monitoring */
static void
dpdk_monitoring_stop(struct vr_interface *monitored_vif,
    struct vr_interface *monitoring_vif)
{
    uint8_t port_id;

    /* check if the monitored vif was reused */
    if (vr_dpdk.monitorings[monitored_vif->vif_idx] != monitoring_vif->vif_idx)
        return;

    /* clear vif flag */
    monitored_vif->vif_flags &= ~((unsigned int)VIF_FLAG_MONITORED);
    rte_wmb();

    /* clear monitoring redirection */
    vr_dpdk.monitorings[monitored_vif->vif_idx] = VR_MAX_INTERFACES;

    if(vif_is_fabric(monitored_vif)) {
        port_id = (((struct vr_dpdk_ethdev *)(monitored_vif->vif_os))->ethdev_port_id);
        rte_eth_promiscuous_disable(port_id);
    }
}

/* Add monitoring interface */
static int
dpdk_monitoring_if_add(struct vr_interface *vif)
{
    int ret;
    unsigned short monitored_vif_id = vif->vif_os_idx;
    struct vr_interface *monitored_vif;
    struct vrouter *router = vrouter_get(vif->vif_rid);

    RTE_LOG(INFO, VROUTER, "Adding monitoring vif %u KNI device %s"
                " to monitor vif %u\n",
                vif->vif_idx, vif->vif_name, monitored_vif_id);

    /* Check if vif exist.
     * We don't need vif reference in order to monitor it.
     * We use the VIF_FLAG_MONITORED to copy in/out packet to the
     * monitoring interface. If the monitored vif get deleted, we simply
     * get no more packets.
     */
    monitored_vif = __vrouter_get_interface(router, monitored_vif_id);
    if (!monitored_vif) {
        RTE_LOG(ERR, VROUTER, "    error getting vif to monitor:"
            " vif %u does not exist\n", monitored_vif_id);
        return -EINVAL;
    }

    /* check if KNI is already added */
    if (vr_dpdk.knis[vif->vif_idx] != NULL) {
        RTE_LOG(ERR, VROUTER, "    error adding monitoring device %s:"
                " vif %d KNI device already exist\n",
                vif->vif_name, vif->vif_idx);
        return -EEXIST;
    }

    /*
     * TODO: we always use DPDK port 0 for monitoring KNI
     * DPDK numerates all the detected Ethernet devices starting from 0.
     * So we might only get into an issue if we have no eth devices at all
     * or we have few eth ports and don't want to use the first one.
     */
    ret = vr_dpdk_knidev_init(0, vif);
    if (ret != 0)
        return ret;

    /* add interface to the table of KNIs */
    vr_dpdk.knis[vif->vif_idx] = vif->vif_os;

    /* write-only interface */
    ret = vr_dpdk_lcore_if_schedule(vif, vr_dpdk_lcore_least_used_get(),
            0, NULL,
            1, &vr_dpdk_kni_tx_queue_init);
    if (ret != 0)
        return ret;

    /* start monitoring */
    dpdk_monitoring_start(monitored_vif, vif);

    return 0;
}

/* Delete monitoring interface */
static int
dpdk_monitoring_if_del(struct vr_interface *vif)
{
    unsigned short monitored_vif_id = vif->vif_os_idx;
    struct vr_interface *monitored_vif;

    RTE_LOG(INFO, VROUTER, "Deleting monitoring vif %u KNI device"
                " to monitor vif %u\n",
                vif->vif_idx, monitored_vif_id);

    /* check if vif exist */
    monitored_vif = __vrouter_get_interface(vrouter_get(vif->vif_rid),
                                                    monitored_vif_id);
    if (!monitored_vif) {
        RTE_LOG(ERR, VROUTER, "    error getting vif to monitor:"
            " vif %u does not exist\n", monitored_vif_id);
    } else {
        /* stop monitoring */
        dpdk_monitoring_stop(monitored_vif, vif);
    }

    vr_dpdk_lcore_if_unschedule(vif);

    /* check if KNI is added */
    if (vr_dpdk.knis[vif->vif_idx] == NULL) {
        RTE_LOG(ERR, VROUTER, "    error deleting monitoring device:"
                " vif %d KNI device does not exist\n",
                vif->vif_idx);
        return -EEXIST;
    }

    /* del the interface from the table of KNIs */
    vr_dpdk.knis[vif->vif_idx] = NULL;

    /* release KNI */
    return vr_dpdk_knidev_release(vif);
}

/* Add agent interface */
static int
dpdk_agent_if_add(struct vr_interface *vif)
{
    int ret;

    RTE_LOG(INFO, VROUTER, "Adding vif %u packet device %s\n",
                vif->vif_idx, vif->vif_name);

    /* check if packet device is already added */
    if (vr_dpdk.packet_ring != NULL) {
        RTE_LOG(ERR, VROUTER, "    error adding packet device %s: already exist\n",
            vif->vif_name);
        return -EEXIST;
    }

    /* init packet device */
    ret = dpdk_packet_socket_init();
    if (ret < 0) {
        RTE_LOG(ERR, VROUTER, "    error initializing packet socket: %s (%d)\n",
            rte_strerror(errno), errno);
        return ret;
    }

    vr_usocket_attach_vif(vr_dpdk.packet_transport, vif);

    /* schedule packet device with no hardware queues */
    return vr_dpdk_lcore_if_schedule(vif, VR_DPDK_PACKET_LCORE_ID, 0, NULL, 0, NULL);
}

/* Delete agent interface */
static int
dpdk_agent_if_del(struct vr_interface *vif)
{
    RTE_LOG(INFO, VROUTER, "Deleting vif %u packet device\n",
                vif->vif_idx);

    dpdk_packet_socket_close();

    return 0;
}

extern void vhost_remove_xconnect(void);

/* vRouter callback */
static int
dpdk_if_add(struct vr_interface *vif)
{
    if (vr_dpdk_is_stop_flag_set())
        return -EINPROGRESS;

    if (vif_is_fabric(vif)) {
        return dpdk_fabric_if_add(vif);
    } else if (vif_is_virtual(vif)) {
        return dpdk_virtual_if_add(vif);
    } else if (vif_is_vhost(vif)) {
        return dpdk_vhost_if_add(vif);
    } else if (vif->vif_type == VIF_TYPE_AGENT) {
        if (vif->vif_transport == VIF_TRANSPORT_SOCKET)
            return dpdk_agent_if_add(vif);

        RTE_LOG(ERR, VROUTER, "Error adding vif %d packet device %s: "
                "unsupported transport %d\n",
                vif->vif_idx, vif->vif_name, vif->vif_transport);
        return -EFAULT;
    } else if (vif->vif_type == VIF_TYPE_MONITORING) {
        return dpdk_monitoring_if_add(vif);
    }

    RTE_LOG(ERR, VROUTER, "Error adding vif %d (%s): unsupported interface type %d\n",
            vif->vif_idx, vif->vif_name, vif->vif_type);

    return -EFAULT;
}

static int
dpdk_if_del(struct vr_interface *vif)
{
    if (vr_dpdk_is_stop_flag_set())
        return -EINPROGRESS;

    if (vif_is_fabric(vif)) {
        return dpdk_fabric_if_del(vif);
    } else if (vif_is_virtual(vif)) {
        return dpdk_virtual_if_del(vif);
    } else if (vif_is_vhost(vif)) {
        return dpdk_vhost_if_del(vif);
    } else if (vif->vif_type == VIF_TYPE_AGENT) {
        if (vif->vif_transport == VIF_TRANSPORT_SOCKET)
            return dpdk_agent_if_del(vif);
    } else if (vif->vif_type == VIF_TYPE_MONITORING) {
        return dpdk_monitoring_if_del(vif);
    }

    RTE_LOG(ERR, VROUTER, "Unsupported interface type %d index %d\n",
            vif->vif_type, vif->vif_idx);

    return -EFAULT;
}

/* vRouter callback */
static int
dpdk_if_del_tap(struct vr_interface *vif)
{
    /* TODO: we untap interfaces at if_del */
    return 0;
}


/* vRouter callback */
static int
dpdk_if_add_tap(struct vr_interface *vif)
{
    /* TODO: we tap interfaces at if_add */
    return 0;
}

static inline void
dpdk_hw_checksum_at_offset(struct vr_packet *pkt, unsigned offset)
{
    struct rte_mbuf *m = vr_dpdk_pkt_to_mbuf(pkt);
    struct vr_ip *iph = NULL;
    struct vr_ip6 *ip6h = NULL;
    unsigned char iph_len = 0, iph_proto = 0;
    struct vr_tcp *tcph;
    struct vr_udp *udph;

    RTE_VERIFY(0 < offset);

    if (pkt->vp_type == VP_TYPE_IP || pkt->vp_type == VP_TYPE_IPOIP) {
        iph = (struct vr_ip *)pkt_data_at_offset(pkt, offset);
        iph_len = iph->ip_hl * 4;
        iph_proto = iph->ip_proto;
        m->ol_flags |= PKT_TX_IP_CKSUM | PKT_TX_IPV4;
        iph->ip_csum = 0;
    } else if (pkt->vp_type == VP_TYPE_IP6 || pkt->vp_type == VP_TYPE_IP6OIP) {
        ip6h = (struct vr_ip6 *)pkt_data_at_offset(pkt, offset);
        iph_len = sizeof(struct vr_ip6);
        iph_proto = ip6h->ip6_nxt;
        m->ol_flags |= PKT_TX_IPV6;
    } else {
        /* Nothing to do if the packet is neither IPv4 nor IPv6. */
        return;
    }

    /* Note: Intel NICs need the checksum set to zero
     * and proper l2/l3 lens to be set.
     */
    m->l3_len = iph_len;
    m->l2_len = offset - rte_pktmbuf_headroom(m);

    /* calculate TCP/UDP checksum */
    if (likely(iph_proto == VR_IP_PROTO_UDP)) {
        m->ol_flags |= PKT_TX_UDP_CKSUM;
        udph = (struct vr_udp *)pkt_data_at_offset(pkt, offset + iph_len);
        udph->udp_csum = 0;
        if (iph)
            udph->udp_csum = rte_ipv4_phdr_cksum((struct ipv4_hdr *)iph, m->ol_flags);
        else if (ip6h)
            udph->udp_csum = rte_ipv6_phdr_cksum((struct ipv6_hdr *)ip6h, m->ol_flags);
    } else if (likely(iph_proto == VR_IP_PROTO_TCP)) {
        m->ol_flags |= PKT_TX_TCP_CKSUM;
        tcph = (struct vr_tcp *)pkt_data_at_offset(pkt, offset + iph_len);
        tcph->tcp_csum = 0;
        if (iph)
            tcph->tcp_csum = rte_ipv4_phdr_cksum((struct ipv4_hdr *)iph, m->ol_flags);
        else if (ip6h)
            tcph->tcp_csum = rte_ipv6_phdr_cksum((struct ipv6_hdr *)ip6h, m->ol_flags);
    }
}

static inline void
dpdk_ipv4_sw_iphdr_checksum_at_offset(struct vr_packet *pkt, unsigned offset)
{
    struct vr_ip *iph;

    RTE_VERIFY(0 < offset);

    iph = (struct vr_ip *)pkt_data_at_offset(pkt, offset);
    iph->ip_csum = vr_ip_csum(iph);
}

static inline void
dpdk_sw_checksum_at_offset(struct vr_packet *pkt, unsigned offset)
{
    struct vr_ip *iph = NULL;
    struct vr_ip6 *ip6h = NULL;
    unsigned char iph_len = 0, iph_proto = 0;
    struct vr_udp *udph;
    struct vr_tcp *tcph;

    RTE_VERIFY(0 < offset);

    if (pkt->vp_type == VP_TYPE_IP || pkt->vp_type == VP_TYPE_IPOIP) {
        iph = (struct vr_ip *)pkt_data_at_offset(pkt, offset);
        iph->ip_csum = vr_ip_csum(iph);
        iph_len = iph->ip_hl * 4;
        iph_proto = iph->ip_proto;
    } else if (pkt->vp_type == VP_TYPE_IP6 || pkt->vp_type == VP_TYPE_IP6OIP) {
        ip6h = (struct vr_ip6 *)pkt_data_at_offset(pkt, offset);
        iph_len = sizeof(struct vr_ip6);
        iph_proto = ip6h->ip6_nxt;
    } else {
        /* Nothing to do if the packet is neither IPv4 nor IPv6. */
        return;
    }

    if (iph_proto == VR_IP_PROTO_UDP) {
        udph = (struct vr_udp *)pkt_data_at_offset(pkt, offset + iph_len);
        udph->udp_csum = 0;
        if (iph)
            udph->udp_csum = rte_ipv4_udptcp_cksum((struct ipv4_hdr *)iph, udph);
        else if (ip6h)
            udph->udp_csum = rte_ipv6_udptcp_cksum((struct ipv6_hdr *)ip6h, udph);
    } else if (iph_proto == VR_IP_PROTO_TCP) {
        tcph = (struct vr_tcp *)pkt_data_at_offset(pkt, offset + iph_len);
        tcph->tcp_csum = 0;
        if (iph)
            tcph->tcp_csum = rte_ipv4_udptcp_cksum((struct ipv4_hdr *)iph, tcph);
        else if (ip6h)
            tcph->tcp_csum = rte_ipv6_udptcp_cksum((struct ipv6_hdr *)ip6h, tcph);
    }
}

static inline void
dpdk_ipv4_outer_tunnel_hw_checksum(struct vr_packet *pkt)
{
    struct rte_mbuf *m = vr_dpdk_pkt_to_mbuf(pkt);
    unsigned offset = pkt->vp_data + sizeof(struct ether_hdr);
    struct vr_ip *iph = (struct vr_ip *)pkt_data_at_offset(pkt, offset);
    unsigned iph_len = iph->ip_hl * 4;
    struct vr_udp *udph;

    m->ol_flags |= PKT_TX_IP_CKSUM | PKT_TX_IPV4;
    iph->ip_csum = 0;
    m->l3_len = iph_len;
    m->l2_len = offset - rte_pktmbuf_headroom(m);

    if (iph->ip_proto == VR_IP_PROTO_UDP) {
        udph = (struct vr_udp *)pkt_data_at_offset(pkt, offset + iph_len);
        udph->udp_length = htons(pkt_len(pkt));
        udph->udp_csum = 0;
    }
}

static inline void
dpdk_ipv4_outer_tunnel_sw_checksum(struct vr_packet *pkt)
{
    unsigned offset = pkt->vp_data + sizeof(struct ether_hdr);
    struct vr_ip *iph = (struct vr_ip *)pkt_data_at_offset(pkt, offset);
    unsigned iph_len = iph->ip_hl * 4;
    struct vr_udp *udph;

    if (iph->ip_proto == VR_IP_PROTO_UDP) {
        udph = (struct vr_udp *)pkt_data_at_offset(pkt, offset + iph_len);
        udph->udp_length = htons(pkt_len(pkt));
        udph->udp_csum = 0;
    }

    iph->ip_csum = vr_ip_csum(iph);
}

static inline void
dpdk_hw_checksum(struct vr_packet *pkt)
{
    /* if a tunnel */
    if (vr_pkt_type_is_overlay(pkt->vp_type)) {
        /* calculate outer checksum in soft */
        /* TODO: vlan support */
        dpdk_ipv4_sw_iphdr_checksum_at_offset(pkt,
            pkt->vp_data + sizeof(struct ether_hdr));
        /* calculate inner checksum in hardware */
        dpdk_hw_checksum_at_offset(pkt, pkt_get_inner_network_header_off(pkt));
    } else if (VP_TYPE_IP == pkt->vp_type || VP_TYPE_IP6 == pkt->vp_type) {
        /* normal IPv4 or IPv6 packet */
        /* TODO: vlan support */
        dpdk_hw_checksum_at_offset(pkt, pkt->vp_data + sizeof(struct ether_hdr));
    }
}

static inline void
dpdk_sw_checksum(struct vr_packet *pkt)
{
    /* if a tunnel */
    if (vr_pkt_type_is_overlay(pkt->vp_type)) {
        /* calculate outer checksum */
        /* TODO: vlan support */
        dpdk_ipv4_sw_iphdr_checksum_at_offset(pkt,
            pkt->vp_data + sizeof(struct ether_hdr));
        /* calculate inner checksum */
        dpdk_sw_checksum_at_offset(pkt, pkt_get_inner_network_header_off(pkt));
    } else if (VP_TYPE_IP == pkt->vp_type || VP_TYPE_IP6 == pkt->vp_type) {
        /* normal IPv4 or IPv6 packet */
        /* TODO: vlan support */
        dpdk_sw_checksum_at_offset(pkt, pkt->vp_data + sizeof(struct ether_hdr));
    }
}

/* TX packet callback */
static int
dpdk_if_tx(struct vr_interface *vif, struct vr_packet *pkt)
{
    const unsigned lcore_id = rte_lcore_id();
    struct vr_dpdk_lcore * const lcore = vr_dpdk.lcores[lcore_id];
    struct rte_mbuf *m = vr_dpdk_pkt_to_mbuf(pkt);
    unsigned vif_idx = vif->vif_idx;
    struct vr_dpdk_queue *tx_queue = &lcore->lcore_tx_queues[vif_idx];
    struct vr_dpdk_queue *monitoring_tx_queue;
    struct vr_packet *p_clone;
    int ret;

    RTE_LOG(DEBUG, VROUTER,"%s: TX packet to interface %s\n", __func__,
        vif->vif_name);

    /* reset mbuf data pointer and length */
    m->data_off = pkt_head_space(pkt);
    m->data_len = pkt_head_len(pkt);
    /* TODO: we do not support mbuf chains */
    m->pkt_len = pkt_head_len(pkt);

    if (unlikely(vif->vif_flags & VIF_FLAG_MONITORED)) {
        monitoring_tx_queue = &lcore->lcore_tx_queues[vr_dpdk.monitorings[vif_idx]];
        if (likely(monitoring_tx_queue && monitoring_tx_queue->txq_ops.f_tx)) {
            p_clone = vr_pclone(pkt);
            if (likely(p_clone != NULL)) {
                monitoring_tx_queue->txq_ops.f_tx(monitoring_tx_queue->q_queue_h,
                    vr_dpdk_pkt_to_mbuf(p_clone));
            }
        }
    }

    if (unlikely(vif->vif_type == VIF_TYPE_AGENT)) {
        ret = rte_ring_mp_enqueue(vr_dpdk.packet_ring, m);
        if (ret != 0) {
            /* TODO: a separate counter for this drop */
            vif_drop_pkt(vif, vr_dpdk_mbuf_to_pkt(m), 0);
            return -1;
        }
#ifdef VR_DPDK_TX_PKT_DUMP
#ifdef VR_DPDK_PKT_DUMP_VIF_FILTER
        if (VR_DPDK_PKT_DUMP_VIF_FILTER(vif))
#endif
        rte_pktmbuf_dump(stdout, m, 0x60);
#endif
        vr_dpdk_packet_wakeup();
        return 0;
    }

    /*
     * With DPDK pktmbufs we don't know if the checksum is incomplete,
     * i.e. there is no direct equivalent of skb->ip_summed field.
     *
     * So we just rely on VP_FLAG_CSUM_PARTIAL flag here, assuming
     * the flag is set when we need to calculate inner or outer packet
     * checksum.
     *
     * This is not elegant and need to be addressed.
     * See dpdk/app/test-pmd/csumonly.c for more checksum examples
     */
    if (unlikely(pkt->vp_flags & VP_FLAG_CSUM_PARTIAL)) {
        /* if NIC supports checksum offload */
        if (likely(vif->vif_flags & VIF_FLAG_TX_CSUM_OFFLOAD))
            dpdk_hw_checksum(pkt);
        else
            dpdk_sw_checksum(pkt);
    } else if (likely(vr_pkt_type_is_overlay(pkt->vp_type))) {
        /* If NIC supports checksum offload.
         * Inner checksum is already done. Compute outer IPv4 checksum,
         * set UDP length, and zero UDP checksum.
         */
        if (likely(vif->vif_flags & VIF_FLAG_TX_CSUM_OFFLOAD)) {
            /* TODO: vlan support */
            dpdk_ipv4_outer_tunnel_hw_checksum(pkt);
        } else {
            /* TODO: vlan support */
            dpdk_ipv4_outer_tunnel_sw_checksum(pkt);
        }
    }

    /* Inject ethertype and vlan tag.
     *
     * Tag only packets that are going to be send to the physical interface,
     * to allow data transfer between compute nodes in the specified VLAN.
     *
     * VLAN tag is adjustable by user with --vlan parameter: see dpdk_vrouter.c.
     * If vRouter is not supposed to work in VLAN (parameter was not specified),
     * packets should not be tagged.
     *
     * TODO: Hardware VLAN tag insert.
     */
    if (vr_dpdk.vlan_tag != VLAN_ID_INVALID && vif_is_fabric(vif)) {
        m->vlan_tci = vr_dpdk.vlan_tag;
        if (rte_vlan_insert(&m)) {
            RTE_LOG(DEBUG, VROUTER,"%s: Error inserting VLAN tag\n", __func__);
            vr_dpdk_pfree(m, VP_DROP_INTERFACE_DROP);
            return -1;
        }
    }

#ifdef VR_DPDK_TX_PKT_DUMP
#ifdef VR_DPDK_PKT_DUMP_VIF_FILTER
    if (VR_DPDK_PKT_DUMP_VIF_FILTER(vif))
#endif
    rte_pktmbuf_dump(stdout, m, 0x60);
#endif

    if (likely(tx_queue->txq_ops.f_tx != NULL)) {
        tx_queue->txq_ops.f_tx(tx_queue->q_queue_h, m);
        if (lcore_id == VR_DPDK_PACKET_LCORE_ID)
            tx_queue->txq_ops.f_flush(tx_queue->q_queue_h);
    } else {
        RTE_LOG(DEBUG, VROUTER,"%s: error TXing to interface %s: no queue for lcore %u\n",
                __func__, vif->vif_name, lcore_id);
        vif_drop_pkt(vif, vr_dpdk_mbuf_to_pkt(m), 0);
        return -1;
    }

    return 0;
}

static int
dpdk_if_rx(struct vr_interface *vif, struct vr_packet *pkt)
{
    const unsigned lcore_id = rte_lcore_id();
    struct vr_dpdk_lcore * const lcore = vr_dpdk.lcores[lcore_id];
    struct rte_mbuf *m = vr_dpdk_pkt_to_mbuf(pkt);
    unsigned vif_idx = vif->vif_idx;
    struct vr_dpdk_queue *tx_queue = &lcore->lcore_tx_queues[vif_idx];
    struct vr_dpdk_queue *monitoring_tx_queue;
    struct vr_packet *p_clone;

    RTE_LOG(DEBUG, VROUTER,"%s: TX packet to interface %s\n", __func__,
        vif->vif_name);

    /* reset mbuf data pointer and length */
    m->data_off = pkt_head_space(pkt);
    m->data_len = pkt_head_len(pkt);
    /* TODO: we do not support mbuf chains */
    m->pkt_len = pkt_head_len(pkt);

    if (unlikely(vif->vif_flags & VIF_FLAG_MONITORED)) {
        monitoring_tx_queue = &lcore->lcore_tx_queues[vr_dpdk.monitorings[vif_idx]];
        if (likely(monitoring_tx_queue && monitoring_tx_queue->txq_ops.f_tx)) {
            p_clone = vr_pclone(pkt);
            if (likely(p_clone != NULL)) {
                monitoring_tx_queue->txq_ops.f_tx(monitoring_tx_queue->q_queue_h,
                    vr_dpdk_pkt_to_mbuf(p_clone));
            }
        }
    }

#ifdef VR_DPDK_TX_PKT_DUMP
#ifdef VR_DPDK_PKT_DUMP_VIF_FILTER
    if (VR_DPDK_PKT_DUMP_VIF_FILTER(vif))
#endif
    rte_pktmbuf_dump(stdout, m, 0x60);
#endif

    if (likely(tx_queue->txq_ops.f_tx != NULL)) {
        tx_queue->txq_ops.f_tx(tx_queue->q_queue_h, m);
    } else {
        RTE_LOG(DEBUG, VROUTER,"%s: error TXing to interface %s: no queue for lcore %u\n",
                __func__, vif->vif_name, lcore_id);
        vif_drop_pkt(vif, vr_dpdk_mbuf_to_pkt(m), 0);
        return -1;
    }

    return 0;
}

static int
dpdk_if_get_settings(struct vr_interface *vif,
        struct vr_interface_settings *settings)
{
    /* TODO: not implemented */
    settings->vis_speed = 1000;
    settings->vis_duplex = 1;
    return 0;
}

static unsigned int
dpdk_if_get_mtu(struct vr_interface *vif)
{
    uint8_t port_id;
    uint16_t mtu;

    if (vif->vif_type == VIF_TYPE_PHYSICAL) {
        port_id = (((struct vr_dpdk_ethdev *)(vif->vif_os))->ethdev_port_id);
        if (rte_eth_dev_get_mtu(port_id, &mtu) == 0)
            return mtu;
    }

    return vif->vif_mtu;
}

static void
dpdk_if_unlock(void)
{
    vr_dpdk_if_unlock();
}

static void
dpdk_if_lock(void)
{
    vr_dpdk_if_lock();
}

static unsigned short
dpdk_if_get_encap(struct vr_interface *vif)
{
    return VIF_ENCAP_TYPE_ETHER;
}

struct vr_host_interface_ops dpdk_interface_ops = {
    .hif_lock           =    dpdk_if_lock,
    .hif_unlock         =    dpdk_if_unlock,
    .hif_add            =    dpdk_if_add,
    .hif_del            =    dpdk_if_del,
    .hif_add_tap        =    dpdk_if_add_tap,   /* not implemented */
    .hif_del_tap        =    dpdk_if_del_tap,   /* not implemented */
    .hif_tx             =    dpdk_if_tx,
    .hif_rx             =    dpdk_if_rx,
    .hif_get_settings   =    dpdk_if_get_settings, /* always returns speed 1000 duplex 1 */
    .hif_get_mtu        =    dpdk_if_get_mtu,
    .hif_get_encap      =    dpdk_if_get_encap, /* always returns VIF_ENCAP_TYPE_ETHER */
};

void
vr_host_vif_init(struct vrouter *router)
{
    return;
}

struct vr_host_interface_ops *
vr_host_interface_init(void)
{
    return &dpdk_interface_ops;
}

void
vr_host_interface_exit(void)
{
    return;
}
