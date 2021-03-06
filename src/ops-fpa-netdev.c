/*
 *  Copyright (C) 2016, Marvell International Ltd. ALL RIGHTS RESERVED.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License"); you may
 *    not use this file except in compliance with the License. You may obtain
 *    a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 *
 *    THIS CODE IS PROVIDED ON AN *AS IS* BASIS, WITHOUT WARRANTIES OR
 *    CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT
 *    LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS
 *    FOR A PARTICULAR PURPOSE, MERCHANTABILITY OR NON-INFRINGEMENT.
 *
 *    See the Apache Version 2.0 License for specific language governing
 *    permissions and limitations under the License.
 */

#include <netdev-provider.h>
#include <ofp-parse.h>
#include <openswitch-idl.h>
#include <openswitch-dflt.h>
#include <netinet/ether.h>
#include "ops-fpa.h"
#include "ops-fpa-tap.h"

VLOG_DEFINE_THIS_MODULE(ops_fpa_netdev);

struct fpa_netdev {
    struct netdev up;
    struct ovs_mutex mutex;

    /* netdev_list */
    struct ovs_list list_node;

    int sid; /* switch id */
    int pid; /* port id */
    struct eth_addr mac; /* MAC address */

    bool inited;

    bool link_status;
    long long link_resets;

    int tap_fd;

    /* For virtual interfaces we have no sid/pid and thus can't access flags
     * via FPA calls. */
    enum netdev_flags flags;
};

#define FPA_NETDEV(PTR) CONTAINER_OF(PTR, struct fpa_netdev, up)

/* Forward declaration of netdev's classes. */
static struct netdev_class fpa_netdev_system;

static int
ops_fpa_netdev_init(void)
{
    FPA_TRACE_FN();

    return 0;
}

static void
ops_fpa_netdev_run(void)
{
    struct shash devices;
    struct shash_node *node;

    shash_init(&devices);
    netdev_get_devices(&fpa_netdev_system, &devices);
    SHASH_FOR_EACH (node, &devices) {
        struct fpa_netdev *dev = FPA_NETDEV(node->data);

        FPA_PORT_PROPERTIES_STC props = {
            .flags = FPA_PORT_PROPERTIES_CONFIG_FLAG,
            .config = 0
        };

        int err = fpaLibPortPropertiesGet(dev->sid, dev->pid, &props);
        if (err) {
            VLOG_ERR("fpaLibPortPropertiesGet: %s", ops_fpa_strerr(err));
            continue;
        }

        bool link_status = props.config & FPA_PORT_CONFIG_DOWN ? false : true;
        if (link_status != dev->link_status) {
            dev->link_status = link_status;
            if (link_status) dev->link_resets++;
            netdev_change_seq_changed(&dev->up);
        }
    }
}

static void
ops_fpa_netdev_wait(void)
{
}

static struct netdev *
ops_fpa_netdev_alloc(void)
{
    struct fpa_netdev *dev = xzalloc(sizeof *dev);
    return &dev->up;
}

static void
ops_fpa_netdev_dealloc(struct netdev *up)
{
    FPA_TRACE_FN();
    struct fpa_netdev *dev = FPA_NETDEV(up);
    free(dev);
}

static int
ops_fpa_netdev_construct(struct netdev *up)
{
    static atomic_count next_n = ATOMIC_COUNT_INIT(0xaa550000);
    unsigned int n;
    struct fpa_netdev *dev = FPA_NETDEV(up);

    VLOG_INFO("%s<%s,%s>:", __func__, up->netdev_class->type, up->name);

    ovs_mutex_init(&dev->mutex);
    dev->sid = FPA_INVALID_SWITCH_ID;
    dev->pid = FPA_INVALID_INTF_ID;

    n = atomic_count_inc(&next_n);
    dev->mac.ea[0] = 0xaa;
    dev->mac.ea[1] = 0x55;
    dev->mac.ea[2] = n >> 24;
    dev->mac.ea[3] = n >> 16;
    dev->mac.ea[4] = n >> 8;
    dev->mac.ea[5] = n;

    dev->inited = false;

    dev->link_status = false;
    dev->link_resets = 0;
    dev->flags = 0;

    dev->tap_fd = 0;

    return 0;
}

static void
ops_fpa_netdev_destruct(struct netdev *up)
{
    int ret;

    FPA_TRACE_FN();
    struct fpa_netdev *dev = FPA_NETDEV(up);

    if (dev->tap_fd) {
        ret = ops_fpa_tap_if_delete(dev->sid, dev->tap_fd);
        if (ret) {
            VLOG_ERR("Failed to delete TAP interface: %s", dev->up.name);
        }
    }

    bool is_bridge_interface = STR_EQ(up->name, "bridge_normal"); /*TODO need to take correct bridge interface name for our ofproto */

    if(is_bridge_interface) {
        ops_fpa_bridge_delete("bridge_normal");
    }

    ovs_mutex_destroy(&dev->mutex);
}

static int
ops_fpa_netdev_get_config(const struct netdev *up, struct smap *args)
{
    FPA_TRACE_FN();

    return 0;
}

static int
ops_fpa_netdev_set_config(struct netdev *up, const struct smap *args)
{
    FPA_TRACE_FN();

    return 0;
}

static int
ops_fpa_netdev_set_hw_intf_info(struct netdev *up, const struct smap *args)
{
    struct fpa_netdev *dev = FPA_NETDEV(up);
    VLOG_INFO("%s<%s,%s>:", __func__, up->netdev_class->type, up->name);

    ovs_mutex_lock(&dev->mutex);

    if (!dev->inited) {
        if (STR_EQ(up->netdev_class->type, "system")) {
            int ret;

            const char *sid = smap_get(args, INTERFACE_HW_INTF_INFO_MAP_SWITCH_UNIT);
            if (ops_fpa_str2int(sid, &dev->sid)) {
                VLOG_ERR("bad %s: %s", INTERFACE_HW_INTF_INFO_MAP_SWITCH_UNIT, sid);
                goto error;
            }
            ovs_assert(dev->sid != FPA_INVALID_SWITCH_ID);

            const char *pid = smap_get(args, INTERFACE_HW_INTF_INFO_MAP_SWITCH_INTF_ID);
            if (ops_fpa_str2int(pid, &dev->pid)) {
                VLOG_ERR("bad %s: %s", INTERFACE_HW_INTF_INFO_MAP_SWITCH_INTF_ID, pid);
                goto error;
            }
            ovs_assert(dev->pid != FPA_INVALID_INTF_ID);

            const char *mac = smap_get(args, INTERFACE_HW_INTF_INFO_MAP_MAC_ADDR);
            if (mac) {
                char * err;
                if ((err = str_to_mac(mac, &dev->mac))) {
                    VLOG_ERR("bad %s: %s", INTERFACE_HW_INTF_INFO_MAP_MAC_ADDR, err);
                    free(err);
                    goto error;
                }
            }

            ret = ops_fpa_tap_if_create(dev->sid, dev->pid, up->name,
                                        ether_aton(mac), &dev->tap_fd);
            if (ret) {
                VLOG_ERR("Failed to create TAP interface: %s", dev->up.name);
                goto error;
            }
        } else {
            VLOG_ERR("Unknown netdev class type: %s", up->netdev_class->type);
        }

        dev->inited = true;
    }

    ovs_mutex_unlock(&dev->mutex);

    return 0;

error:
    ovs_mutex_unlock(&dev->mutex);

    return EINVAL;
}


static uint32_t
get_interface_speed_features(const struct smap *args)
{
    bool duplex = STR_EQ(smap_get(args, INTERFACE_HW_INTF_CONFIG_MAP_DUPLEX),
                         INTERFACE_HW_INTF_CONFIG_MAP_DUPLEX_FULL);

    int speed = 1000;
    const char * str = smap_get(args, INTERFACE_HW_INTF_CONFIG_MAP_SPEEDS);
    if (ops_fpa_str2int(str, &speed)) {
        VLOG_WARN("%s: invalid %s: %s", __func__, INTERFACE_HW_INTF_CONFIG_MAP_SPEEDS, str);
    }

    switch (speed) {
    case 10:      return duplex ? FPA_PORT_FEAT_10MB_FD : FPA_PORT_FEAT_10MB_HD;
    case 100:     return duplex ? FPA_PORT_FEAT_100MB_FD : FPA_PORT_FEAT_100MB_HD;
    case 1000:    return duplex ? FPA_PORT_FEAT_1GB_FD : FPA_PORT_FEAT_1GB_HD;
    case 2500:    return FPA_PORT_FEAT_2_5GB_FD;
    case 10000:   return FPA_PORT_FEAT_10GB_FD;
    case 20000:   return FPA_PORT_FEAT_20GB_FD;
    case 40000:   return FPA_PORT_FEAT_40GB_FD;
    case 100000:  return FPA_PORT_FEAT_100GB_FD;
    case 1000000: return FPA_PORT_FEAT_1TB_FD;
    default:
        /* Set 1G as default speed */
        return duplex ? FPA_PORT_FEAT_1GB_FD : FPA_PORT_FEAT_1GB_HD;
    }
}

static uint32_t
get_interface_autoneg_features(const struct smap *args)
{
    bool autoneg = STR_EQ(smap_get(args, INTERFACE_HW_INTF_CONFIG_MAP_AUTONEG),
                          INTERFACE_HW_INTF_CONFIG_MAP_AUTONEG_ON);
    return autoneg ? FPA_PORT_FEAT_AUTONEG : 0;
}

static uint32_t
get_interface_pause_features(const struct smap *args)
{
    const char *pause = smap_get(args, INTERFACE_HW_INTF_CONFIG_MAP_PAUSE);

    if (STR_EQ(pause, INTERFACE_HW_INTF_CONFIG_MAP_PAUSE_RX)) {
        return FPA_PORT_FEAT_PAUSE;
    } else if (STR_EQ(pause, INTERFACE_HW_INTF_CONFIG_MAP_PAUSE_TX)) {
        return FPA_PORT_FEAT_PAUSE_ASYM;
    } else if (STR_EQ(pause, INTERFACE_HW_INTF_CONFIG_MAP_PAUSE_RXTX)) {
        return FPA_PORT_FEAT_PAUSE | FPA_PORT_FEAT_PAUSE_ASYM;
    }

    return 0;
}

static int
ops_fpa_netdev_set_hw_intf_config(struct netdev *up, const struct smap *args)
{
    struct fpa_netdev *dev = FPA_NETDEV(up);
    FPA_PORT_PROPERTIES_STC props;
    int err;

    VLOG_INFO("%s<%s,%s>:", __func__, up->netdev_class->type, up->name);

    if (!dev->inited) {
        VLOG_WARN("netdev interface %s is not initialized.", up->name);
        return EPERM;
    }

    struct smap_node *node;
    SMAP_FOR_EACH(node, args) {
        VLOG_INFO("    %s=%s", node->key, node->value);
    }

    bool enable = smap_get_bool(args, INTERFACE_HW_INTF_CONFIG_MAP_ENABLE, false);

    /* Get the bimap of supported features. */
    props.flags = FPA_PORT_PROPERTIES_SUPPORTED_FLAG;
    err = fpaLibPortPropertiesGet(dev->sid, dev->pid, &props);
    if (err) {
        VLOG_ERR("fpaLibPortPropertiesGet: %s", ops_fpa_strerr(err));
        return EINVAL;
    }

    if (enable) {
        props.flags = FPA_PORT_PROPERTIES_CONFIG_FLAG;
        props.config = 0;
        props.featuresBmp =  get_interface_speed_features(args);
        props.featuresBmp |= get_interface_autoneg_features(args);
        props.featuresBmp |= get_interface_pause_features(args);

        /* Drop all unsupported features. */
        props.featuresBmp &= props.supportedBmp;

        /* Set features only if they are present */
        if (props.featuresBmp != 0)
            props.flags |= FPA_PORT_PROPERTIES_FEATURES_FLAG;

        dev->flags = NETDEV_UP;
    } else {
        /* Features are absent when interface goes shutdown */
        props.flags = FPA_PORT_PROPERTIES_CONFIG_FLAG;
        props.config = FPA_PORT_CONFIG_DOWN;

        dev->flags = 0;
    }

    err = wrap_fpaLibPortPropertiesSet(dev->sid, dev->pid, &props);
    if (err) {
        VLOG_ERR("wrap_fpaLibPortPropertiesSet: %s", ops_fpa_strerr(err));

        return EINVAL;
    }

    netdev_change_seq_changed(up);

    return 0;
}

static int
ops_fpa_netdev_set_etheraddr(struct netdev *up, const struct eth_addr mac)
{
    struct fpa_netdev *dev = FPA_NETDEV(up);

    ovs_mutex_lock(&dev->mutex);

    if (memcmp(dev->mac.ea, mac.ea, ETH_ADDR_LEN)) {
        VLOG_INFO("%s<%s,%s>: mac="ETH_ADDR_FMT,
                  __func__, netdev_get_type(up),
                  netdev_get_name(up), ETH_ADDR_ARGS(mac));
        memcpy(dev->mac.ea, mac.ea, ETH_ADDR_LEN);
        netdev_change_seq_changed(up);
    }

    ovs_mutex_unlock(&dev->mutex);

    return 0;
}

static int
ops_fpa_netdev_get_etheraddr(const struct netdev *up, struct eth_addr *mac)
{
    struct fpa_netdev *dev = FPA_NETDEV(up);
    ovs_mutex_lock(&dev->mutex);
    *mac = dev->mac;
    ovs_mutex_unlock(&dev->mutex);

    return 0;
}

static int
ops_fpa_netdev_get_mtu(const struct netdev *up, int *mtup)
{
    FPA_TRACE_FN();

    return EOPNOTSUPP;
}

static int
ops_fpa_netdev_set_mtu(const struct netdev *up, int mtu)
{
    FPA_TRACE_FN();

    return EOPNOTSUPP;
}

static int
ops_fpa_netdev_get_ifindex(const struct netdev *up)
{
    struct fpa_netdev *dev = FPA_NETDEV(up);

    return dev->pid;
}

static int
ops_fpa_netdev_get_carrier(const struct netdev *up, bool *carrier)
{
    struct fpa_netdev *dev = FPA_NETDEV(up);
    *carrier = dev->link_status;

    return 0;
}

static long long int
ops_fpa_netdev_get_carrier_resets(const struct netdev *up)
{
    struct fpa_netdev *dev = FPA_NETDEV(up);

    return dev->link_resets;
}

static int
ops_fpa_netdev_set_miimon_interval(struct netdev *up, long long int interval)
{
    FPA_TRACE_FN();

    return 0;
}

static int
ops_fpa_netdev_get_stats(const struct netdev *up, struct netdev_stats *stats)
{
    VLOG_DBG("%s<%s,%s>:", __func__, up->netdev_class->type, up->name);
    struct fpa_netdev *dev = FPA_NETDEV(up);

    if (STR_EQ(up->netdev_class->type, "internal")) {
        memset(stats, 0xFF, sizeof(*stats));
        return 0;
    }

    FPA_PORT_COUNTERS_STC counters;
    int err = fpaLibPortStatisticsGet(dev->sid, dev->pid, &counters);
    if (err) {
        VLOG_ERR("fpaLibPortStatisticsGet: %s", ops_fpa_strerr(err));

        return EINVAL;
    }

    memset(stats, 0xFF, sizeof(*stats));

    stats->rx_packets      = counters.rxPackets;
    stats->tx_packets      = counters.txPackets;
    stats->rx_bytes        = counters.rxBytes;
    stats->tx_bytes        = counters.txBytes;
    stats->rx_dropped      = counters.rxDropped;
    stats->tx_dropped      = counters.txDropped;
    stats->rx_errors       = counters.rxErrors;
    stats->tx_errors       = counters.txErrors;
    stats->rx_frame_errors = counters.rxFrameErr;
    stats->rx_fifo_errors  = counters.rxOverErr;
    stats->rx_crc_errors   = counters.rxCrcErr;
    stats->collisions      = counters.collisions;

    return 0;
}

static enum netdev_features
fpa_to_netdev_features(uint32_t f)
{
    enum netdev_features n = 0;

    if (f & FPA_PORT_FEAT_10MB_HD)    n |= NETDEV_F_10MB_HD;
    if (f & FPA_PORT_FEAT_10MB_FD)    n |= NETDEV_F_10MB_FD;
    if (f & FPA_PORT_FEAT_100MB_HD)   n |= NETDEV_F_100MB_HD;
    if (f & FPA_PORT_FEAT_100MB_FD)   n |= NETDEV_F_100MB_FD;
    if (f & FPA_PORT_FEAT_1GB_HD)     n |= NETDEV_F_1GB_HD;
    if (f & FPA_PORT_FEAT_1GB_FD)     n |= NETDEV_F_1GB_FD;
    if (f & FPA_PORT_FEAT_2_5GB_FD)   n |= NETDEV_F_OTHER;
    if (f & FPA_PORT_FEAT_10GB_FD)    n |= NETDEV_F_10GB_FD;
    if (f & FPA_PORT_FEAT_20GB_FD)    n |= NETDEV_F_OTHER;
    if (f & FPA_PORT_FEAT_40GB_FD)    n |= NETDEV_F_40GB_FD;
    if (f & FPA_PORT_FEAT_100GB_FD)   n |= NETDEV_F_100GB_FD;
    if (f & FPA_PORT_FEAT_1TB_FD)     n |= NETDEV_F_1TB_FD;
    if (f & FPA_PORT_FEAT_OTHER)      n |= NETDEV_F_OTHER;
    if (f & FPA_PORT_FEAT_COPPER)     n |= NETDEV_F_COPPER;
    if (f & FPA_PORT_FEAT_FIBER)      n |= NETDEV_F_FIBER;
    if (f & FPA_PORT_FEAT_AUTONEG)    n |= NETDEV_F_AUTONEG;
    if (f & FPA_PORT_FEAT_PAUSE)      n |= NETDEV_F_PAUSE;
    if (f & FPA_PORT_FEAT_PAUSE_ASYM) n |= NETDEV_F_PAUSE_ASYM;

    return n;
}

static uint32_t
netdev_to_fpa_features(enum netdev_features n)
{
    uint32_t f = 0;

    if (n & NETDEV_F_10MB_HD)    f |= FPA_PORT_FEAT_10MB_HD;
    if (n & NETDEV_F_10MB_FD)    f |= FPA_PORT_FEAT_10MB_FD;
    if (n & NETDEV_F_100MB_HD)   f |= FPA_PORT_FEAT_100MB_HD;
    if (n & NETDEV_F_100MB_FD)   f |= FPA_PORT_FEAT_100MB_FD;
    if (n & NETDEV_F_1GB_HD)     f |= FPA_PORT_FEAT_1GB_HD;
    if (n & NETDEV_F_1GB_FD)     f |= FPA_PORT_FEAT_1GB_FD;
    if (n & NETDEV_F_OTHER)      f |= FPA_PORT_FEAT_2_5GB_FD;
    if (n & NETDEV_F_10GB_FD)    f |= FPA_PORT_FEAT_10GB_FD;
    if (n & NETDEV_F_OTHER)      f |= FPA_PORT_FEAT_20GB_FD;
    if (n & NETDEV_F_40GB_FD)    f |= FPA_PORT_FEAT_40GB_FD;
    if (n & NETDEV_F_100GB_FD)   f |= FPA_PORT_FEAT_100GB_FD;
    if (n & NETDEV_F_1TB_FD)     f |= FPA_PORT_FEAT_1TB_FD;
    if (n & NETDEV_F_OTHER)      f |= FPA_PORT_FEAT_OTHER;
    if (n & NETDEV_F_COPPER)     f |= FPA_PORT_FEAT_COPPER;
    if (n & NETDEV_F_FIBER)      f |= FPA_PORT_FEAT_FIBER;
    if (n & NETDEV_F_AUTONEG)    f |= FPA_PORT_FEAT_AUTONEG;
    if (n & NETDEV_F_PAUSE)      f |= FPA_PORT_FEAT_PAUSE;
    if (n & NETDEV_F_PAUSE_ASYM) f |= FPA_PORT_FEAT_PAUSE_ASYM;

    return f;
}

static int
ops_fpa_netdev_get_features(const struct netdev *up,
                            enum netdev_features *current,
                            enum netdev_features *advertised,
                            enum netdev_features *supported,
                            enum netdev_features *peer)
{
    struct fpa_netdev *dev = FPA_NETDEV(up);
    FPA_PORT_PROPERTIES_STC props;

    int err = fpaLibPortPropertiesGet(dev->sid, dev->pid, &props);
    if (err) {
        VLOG_ERR("fpaLibPortPropertiesGet: %s", ops_fpa_strerr(err));

        return EINVAL;
    }

    *current = props.flags & FPA_PORT_PROPERTIES_FEATURES_FLAG ?
               fpa_to_netdev_features(props.featuresBmp) : 0;
    *advertised = props.flags & FPA_PORT_PROPERTIES_ADVERTISED_FLAG ?
                  fpa_to_netdev_features(props.advertBmp) : 0;
    *supported = props.flags & FPA_PORT_PROPERTIES_SUPPORTED_FLAG ?
                 fpa_to_netdev_features(props.supportedBmp) : 0;
    *peer = props.flags & FPA_PORT_PROPERTIES_PEER_FLAG ?
            fpa_to_netdev_features(props.peerBmp) : 0;

    return 0;
}

static int
ops_fpa_netdev_set_advertisements(struct netdev *up, enum netdev_features advertise)
{
    FPA_TRACE_FN();
    struct fpa_netdev *dev = FPA_NETDEV(up);
    FPA_PORT_PROPERTIES_STC props = {
        .flags = FPA_PORT_PROPERTIES_ADVERTISED_FLAG,
        .advertBmp = netdev_to_fpa_features(advertise)
    };

    int err = wrap_fpaLibPortPropertiesSet(dev->sid, dev->pid, &props);
    if (err) {
        VLOG_ERR("wrap_fpaLibPortPropertiesSet: %s", ops_fpa_strerr(err));
        return EINVAL;
    }

    return 0;
}

static int
ops_fpa_netdev_update_flags(struct netdev *up, enum netdev_flags off,
                            enum netdev_flags on, enum netdev_flags *old_flags)
{
    int err;
    struct fpa_netdev *dev = FPA_NETDEV(up);

    /* It must be a real interface. */
    if (dev->sid == FPA_INVALID_SWITCH_ID) {

        return EOPNOTSUPP;
    }

    FPA_PORT_PROPERTIES_STC props = {
        .flags = FPA_PORT_PROPERTIES_CONFIG_FLAG,
        .config = 0
    };

    /* Get the current state to update old flags */
    err = fpaLibPortPropertiesGet(dev->sid, dev->pid, &props);
    if (err) {
        VLOG_ERR("fpaLibPortPropertiesGet: %s", ops_fpa_strerr(err));
        return EINVAL;
    }

    bool down = props.config & FPA_PORT_CONFIG_DOWN;
    VLOG_DBG("fpaLibPortPropertiesGet: down=%d config=%d flags=%d state=%d",
              down, props.config, props.flags, props.state);
    *old_flags = down ? 0 : NETDEV_UP;

    /* We support only NETDEV_UP */
    if ((on | off) & NETDEV_UP) {
        if (on & NETDEV_UP) {
            props.config &= ~FPA_PORT_CONFIG_DOWN;
        } else if (off & NETDEV_UP) {
            props.config |= FPA_PORT_CONFIG_DOWN;
        }

        err = wrap_fpaLibPortPropertiesSet(dev->sid, dev->pid, &props);
        if (err) {
            VLOG_ERR("wrap_fpaLibPortPropertiesSet: %s", ops_fpa_strerr(err));
            return EINVAL;
        }
    }

    return 0;
}

static struct netdev_class fpa_netdev_system = {
    .type                 = "system",
    .init                 = NULL,
    .run                  = ops_fpa_netdev_run,
    .wait                 = NULL,
    .alloc                = ops_fpa_netdev_alloc,
    .construct            = ops_fpa_netdev_construct,
    .destruct             = ops_fpa_netdev_destruct,
    .dealloc              = ops_fpa_netdev_dealloc,
    .get_config           = NULL,
    .set_config           = NULL,
    .set_hw_intf_info     = ops_fpa_netdev_set_hw_intf_info,
    .set_hw_intf_config   = ops_fpa_netdev_set_hw_intf_config,
    .get_tunnel_config    = NULL,
    .build_header         = NULL,
    .push_header          = NULL,
    .pop_header           = NULL,
    .get_numa_id          = NULL,
    .set_multiq           = NULL,
    .send                 = NULL,
    .send_wait            = NULL,
    .set_etheraddr        = ops_fpa_netdev_set_etheraddr,
    .get_etheraddr        = ops_fpa_netdev_get_etheraddr,
    .get_mtu              = NULL,
    .set_mtu              = NULL,
    .get_ifindex          = ops_fpa_netdev_get_ifindex,
    .get_carrier          = ops_fpa_netdev_get_carrier,
    .get_carrier_resets   = ops_fpa_netdev_get_carrier_resets,
    .set_miimon_interval  = NULL,
    .get_stats            = ops_fpa_netdev_get_stats,
    .get_features         = ops_fpa_netdev_get_features,
    .set_advertisements   = ops_fpa_netdev_set_advertisements,
    .set_policing         = NULL,
    .get_qos_types        = NULL,
    .get_qos_capabilities = NULL,
    .get_qos              = NULL,
    .set_qos              = NULL,
    .get_queue            = NULL,
    .set_queue            = NULL,
    .delete_queue         = NULL,
    .get_queue_stats      = NULL,
    .queue_dump_start     = NULL,
    .queue_dump_next      = NULL,
    .queue_dump_done      = NULL,
    .dump_queue_stats     = NULL,
    .get_in4              = NULL,
    .set_in4              = NULL,
    .get_in6              = NULL,
    .add_router           = NULL,
    .get_next_hop         = NULL,
    .get_status           = NULL,
    .arp_lookup           = NULL,
    .update_flags         = ops_fpa_netdev_update_flags,
    .rxq_alloc            = NULL,
    .rxq_construct        = NULL,
    .rxq_destruct         = NULL,
    .rxq_dealloc          = NULL,
    .rxq_recv             = NULL,
    .rxq_wait             = NULL,
    .rxq_drain            = NULL,
};

static int
internal_netdev_set_hw_intf_info(struct netdev *up, const struct smap *args)
{
    struct fpa_netdev *dev = FPA_NETDEV(up);

    VLOG_INFO("%s<%s,%s>", __func__, up->netdev_class->type, netdev_get_name(up));

    struct smap_node *node;
    SMAP_FOR_EACH (node, args) {
        VLOG_INFO("    %s=%s", node->key, node->value);
    }

    ovs_mutex_lock(&dev->mutex);
    if (!dev->inited) {

        bool is_bridge_interface = smap_get_bool(args, INTERFACE_HW_INTF_INFO_MAP_BRIDGE,
                                                 DFLT_INTERFACE_HW_INTF_INFO_MAP_BRIDGE);

        if(is_bridge_interface) {
            int ret = 0;
            struct ether_addr *ether_mac = NULL;

            ether_mac = (struct ether_addr *) &dev->mac;

            ret = ops_fpa_bridge_create(netdev_get_name(up), ether_mac);
            if (ret) {
                VLOG_ERR("Failed to initialize interface %s", netdev_get_name(up));
                goto error;
            }
        }

        dev->inited = true;
    }
    ovs_mutex_unlock(&dev->mutex);

    return 0;

error:
    ovs_mutex_unlock(&dev->mutex);

    return EINVAL;
}

static int
internal_netdev_set_hw_intf_config(struct netdev *up, const struct smap *args)
{
    FPA_TRACE_FN();
    struct fpa_netdev *dev = FPA_NETDEV(up);

    VLOG_INFO("%s<%s,%s>:", __func__, up->netdev_class->type, netdev_get_name(up));

    struct smap_node *node;
    SMAP_FOR_EACH(node, args) {
        VLOG_INFO("    %s=%s", node->key, node->value);
    }

    if (!dev->inited) {
        VLOG_WARN("netdev interface %s is not initialized.", netdev_get_name(up));
        return EPERM;
    }

    bool enable = smap_get_bool(args, INTERFACE_HW_INTF_CONFIG_MAP_ENABLE, false);

    ovs_mutex_lock(&dev->mutex);
    dev->flags = (enable) ? NETDEV_UP : 0;
    ovs_mutex_unlock(&dev->mutex);

    netdev_change_seq_changed(up);

    return 0;
}

static int
internal_netdev_update_flags(struct netdev *up, enum netdev_flags off,
                             enum netdev_flags on, enum netdev_flags *old_flags)
{
    struct fpa_netdev *dev = FPA_NETDEV(up);

    /* Ignore flags on/off, just return current flags. */
    *old_flags = dev->flags;

    return 0;
}

static struct netdev_class fpa_netdev_internal = {
    .type                 = "internal",
    .init                 = NULL,
    .run                  = NULL,
    .wait                 = NULL,
    .alloc                = ops_fpa_netdev_alloc,
    .construct            = ops_fpa_netdev_construct,
    .destruct             = ops_fpa_netdev_destruct,
    .dealloc              = ops_fpa_netdev_dealloc,
    .get_config           = NULL,
    .set_config           = NULL,
    .set_hw_intf_info     = internal_netdev_set_hw_intf_info,
    .set_hw_intf_config   = internal_netdev_set_hw_intf_config,
    .get_tunnel_config    = NULL,
    .build_header         = NULL,
    .push_header          = NULL,
    .pop_header           = NULL,
    .get_numa_id          = NULL,
    .set_multiq           = NULL,
    .send                 = NULL,
    .send_wait            = NULL,
    .set_etheraddr        = ops_fpa_netdev_set_etheraddr,
    .get_etheraddr        = ops_fpa_netdev_get_etheraddr,
    .get_mtu              = NULL,
    .set_mtu              = NULL,
    .get_ifindex          = ops_fpa_netdev_get_ifindex,
    .get_carrier          = NULL,
    .get_carrier_resets   = NULL,
    .set_miimon_interval  = NULL,
    .get_stats            = NULL,
    .get_features         = NULL,
    .set_advertisements   = NULL,
    .set_policing         = NULL,
    .get_qos_types        = NULL,
    .get_qos_capabilities = NULL,
    .get_qos              = NULL,
    .set_qos              = NULL,
    .get_queue            = NULL,
    .set_queue            = NULL,
    .delete_queue         = NULL,
    .get_queue_stats      = NULL,
    .queue_dump_start     = NULL,
    .queue_dump_next      = NULL,
    .queue_dump_done      = NULL,
    .dump_queue_stats     = NULL,
    .get_in4              = NULL,
    .set_in4              = NULL,
    .get_in6              = NULL,
    .add_router           = NULL,
    .get_next_hop         = NULL,
    .get_status           = NULL,
    .arp_lookup           = NULL,
    .update_flags         = internal_netdev_update_flags,
    .rxq_alloc            = NULL,
    .rxq_construct        = NULL,
    .rxq_destruct         = NULL,
    .rxq_dealloc          = NULL,
    .rxq_recv             = NULL,
    .rxq_wait             = NULL,
    .rxq_drain            = NULL
};

static struct netdev_class fpa_netdev_vlansubint = {
    .type                 = "vlansubint",
    .init                 = NULL,
    .run                  = NULL,
    .wait                 = NULL,
    .alloc                = ops_fpa_netdev_alloc,
    .construct            = ops_fpa_netdev_construct,
    .destruct             = ops_fpa_netdev_destruct,
    .dealloc              = ops_fpa_netdev_dealloc,
    .get_config           = NULL,
    .set_config           = NULL,
    .set_hw_intf_info     = ops_fpa_netdev_set_hw_intf_info,
    .set_hw_intf_config   = ops_fpa_netdev_set_hw_intf_config,
    .get_tunnel_config    = NULL,
    .build_header         = NULL,
    .push_header          = NULL,
    .pop_header           = NULL,
    .get_numa_id          = NULL,
    .set_multiq           = NULL,
    .send                 = NULL,
    .send_wait            = NULL,
    .set_etheraddr        = ops_fpa_netdev_set_etheraddr,
    .get_etheraddr        = ops_fpa_netdev_get_etheraddr,
    .get_mtu              = NULL,
    .set_mtu              = NULL,
    .get_ifindex          = NULL,
    .get_carrier          = ops_fpa_netdev_get_carrier,
    .get_carrier_resets   = NULL,
    .set_miimon_interval  = NULL,
    .get_stats            = NULL,
    .get_features         = NULL,
    .set_advertisements   = NULL,
    .set_policing         = NULL,
    .get_qos_types        = NULL,
    .get_qos_capabilities = NULL,
    .get_qos              = NULL,
    .set_qos              = NULL,
    .get_queue            = NULL,
    .set_queue            = NULL,
    .delete_queue         = NULL,
    .get_queue_stats      = NULL,
    .queue_dump_start     = NULL,
    .queue_dump_next      = NULL,
    .queue_dump_done      = NULL,
    .dump_queue_stats     = NULL,
    .get_in4              = NULL,
    .set_in4              = NULL,
    .get_in6              = NULL,
    .add_router           = NULL,
    .get_next_hop         = NULL,
    .get_status           = NULL,
    .arp_lookup           = NULL,
    .update_flags         = ops_fpa_netdev_update_flags,
    .rxq_alloc            = NULL,
    .rxq_construct        = NULL,
    .rxq_destruct         = NULL,
    .rxq_dealloc          = NULL,
    .rxq_recv             = NULL,
    .rxq_wait             = NULL,
    .rxq_drain            = NULL
};

static struct netdev_class fpa_netdev_l3_loopback = {
    .type                 = "loopback",
    .init                 = NULL,
    .run                  = NULL,
    .wait                 = NULL,
    .alloc                = ops_fpa_netdev_alloc,
    .construct            = ops_fpa_netdev_construct,
    .destruct             = ops_fpa_netdev_destruct,
    .dealloc              = ops_fpa_netdev_dealloc,
    .get_config           = NULL,
    .set_config           = NULL,
    .set_hw_intf_info     = NULL,
    .set_hw_intf_config   = NULL,
    .get_tunnel_config    = NULL,
    .build_header         = NULL,
    .push_header          = NULL,
    .pop_header           = NULL,
    .get_numa_id          = NULL,
    .set_multiq           = NULL,
    .send                 = NULL,
    .send_wait            = NULL,
    .set_etheraddr        = ops_fpa_netdev_set_etheraddr,
    .get_etheraddr        = ops_fpa_netdev_get_etheraddr,
    .get_mtu              = NULL,
    .set_mtu              = NULL,
    .get_ifindex          = NULL,
    .get_carrier          = NULL,
    .get_carrier_resets   = NULL,
    .set_miimon_interval  = NULL,
    .get_stats            = NULL,
    .get_features         = NULL,
    .set_advertisements   = NULL,
    .set_policing         = NULL,
    .get_qos_types        = NULL,
    .get_qos_capabilities = NULL,
    .get_qos              = NULL,
    .set_qos              = NULL,
    .get_queue            = NULL,
    .set_queue            = NULL,
    .delete_queue         = NULL,
    .get_queue_stats      = NULL,
    .queue_dump_start     = NULL,
    .queue_dump_next      = NULL,
    .queue_dump_done      = NULL,
    .dump_queue_stats     = NULL,
    .get_in4              = NULL,
    .set_in4              = NULL,
    .get_in6              = NULL,
    .add_router           = NULL,
    .get_next_hop         = NULL,
    .get_status           = NULL,
    .arp_lookup           = NULL,
    .update_flags         = internal_netdev_update_flags,
    .rxq_alloc            = NULL,
    .rxq_construct        = NULL,
    .rxq_destruct         = NULL,
    .rxq_dealloc          = NULL,
    .rxq_recv             = NULL,
    .rxq_wait             = NULL,
    .rxq_drain            = NULL
};

void
netdev_register(void)
{
    netdev_register_provider(&fpa_netdev_system);
    netdev_register_provider(&fpa_netdev_internal);
    netdev_register_provider(&fpa_netdev_vlansubint);
    netdev_register_provider(&fpa_netdev_l3_loopback);
}
