/* packet-iso15765.c
 * Routines for iso15765 protocol packet disassembly
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * CAN ID Mapping
 *
 * When using ISO15765 to transport UDS and others, the diagnostic addresses might be
 * determined by mapping the underlaying CAN ID (29bit or 11bit).
 *
 * Option 1: Two Addresses can be determined (Source and Target Address.
 * Option 2: One Address can be determined (ECU Address).
 * Option 3: No Address can be determined.
 *
 * For Option 1 and 2 the ISO15765_can_id_mappings table can be used to determine the addresses:
 * - Ext Addr determines, if the CAN ID is 29bit (TRUE) or 11bit (FALSE)
 * - CAN ID and CAN ID Mask determined how to know if a CAN ID should be mapped
 * - Source Addr Mask and Target Addr Mask show the bits used to determine the addresses of Option 1
 * - ECU Addr Mask defines the bits for the address of Option 2
 *
 * Example:
 * - ISO15765 is applicable to all 29bit CAN IDs 0x9988TTSS, with TT the target address and SS the source address.
 * - Ext Addr: TRUE
 * - CAN ID: 0x99880000
 * - CAN ID Mask: 0xffff0000
 * - Target Addr Mask: 0x0000ff00
 * - Source Addr Mask: 0x000000ff
 *
 * The addresses are passed via iso15765data_t to the next dissector (e.g., UDS).
 */

/*
 * Support for FlexRay variant, see: https://www.autosar.org/fileadmin/user_upload/standards/classic/20-11/AUTOSAR_SWS_FlexRayARTransportLayer.pdf
 */

#include "config.h"

#include <epan/packet.h>
#include <epan/prefs.h>
#include <epan/decode_as.h>
#include <epan/reassemble.h>
#include <epan/expert.h>
#include <epan/proto_data.h>
#include <epan/uat.h>
#include <wsutil/bits_ctz.h>

#include "packet-socketcan.h"
#include "packet-lin.h"
#include "packet-flexray.h"
#include "packet-iso15765.h"
#include "packet-autosar-ipdu-multiplexer.h"
#include "packet-pdu-transport.h"

void proto_register_iso15765(void);
void proto_reg_handoff_iso15765(void);

#define ISO15765_PCI_LEN 1
#define ISO15765_PCI_FD_SF_LEN 2
#define ISO15765_PCI_FD_FF_LEN 6

#define ISO15765_MESSAGE_TYPE_MASK 0xF0
#define ISO15765_MESSAGE_TYPES_SINGLE_FRAME 0
#define ISO15765_MESSAGE_TYPES_FIRST_FRAME 1
#define ISO15765_MESSAGE_TYPES_CONSECUTIVE_FRAME 2
#define ISO15765_MESSAGE_TYPES_FLOW_CONTROL 3
#define ISO15765_MESSAGE_TYPES_FR_SINGLE_FRAME_EXT 4
#define ISO15765_MESSAGE_TYPES_FR_FIRST_FRAME_EXT 5
#define ISO15765_MESSAGE_TYPES_FR_CONSECUTIVE_FRAME_2 6
#define ISO15765_MESSAGE_TYPES_FR_ACK_FRAME 7

#define ISO15765_MESSAGE_DATA_LENGTH_MASK 0x0F
#define ISO15765_FD_MESSAGE_DATA_LENGTH_MASK 0x00FF
#define ISO15765_MESSAGE_EXTENDED_FRAME_LENGTH_MASK 0x0F
#define ISO15765_MESSAGE_FRAME_LENGTH_OFFSET (ISO15765_PCI_LEN)
#define ISO15765_MESSAGE_FRAME_LENGTH_LEN 1
#define ISO15765_MESSAGE_SEQUENCE_NUMBER_MASK 0x0F
#define ISO15765_MESSAGE_FLOW_STATUS_MASK 0x0F

#define ISO15765_FC_BS_OFFSET (ISO15765_PCI_LEN)
#define ISO15765_FC_BS_LEN 1
#define ISO15765_FC_STMIN_OFFSET (ISO15765_FC_BS_OFFSET + ISO15765_FC_BS_LEN)
#define ISO15765_FC_STMIN_LEN 1

#define ISO15765_MESSAGE_AUTOSAR_ACK_MASK 0xF0
#define ISO15765_AUTOSAR_ACK_OFFSET 3

#define ISO15765_ADDR_INVALID 0xffffffff

typedef struct iso15765_identifier {
    guint32 id;
    guint32 seq;
    guint16 frag_id;
    gboolean last;
} iso15765_identifier_t;

typedef struct iso15765_frame {
    guint32  seq;
    guint32  offset;
    guint32  len;
    gboolean error;
    gboolean complete;
    guint16  last_frag_id;
    guint8   frag_id_high[16];
} iso15765_frame_t;

static const value_string iso15765_message_types[] = {
    {ISO15765_MESSAGE_TYPES_SINGLE_FRAME, "Single Frame"},
    {ISO15765_MESSAGE_TYPES_FIRST_FRAME, "First Frame"},
    {ISO15765_MESSAGE_TYPES_CONSECUTIVE_FRAME, "Consecutive Frame"},
    {ISO15765_MESSAGE_TYPES_FLOW_CONTROL, "Flow control"},
    {ISO15765_MESSAGE_TYPES_FR_SINGLE_FRAME_EXT, "Single Frame Ext"},
    {ISO15765_MESSAGE_TYPES_FR_FIRST_FRAME_EXT, "First Frame Ext"},
    {ISO15765_MESSAGE_TYPES_FR_CONSECUTIVE_FRAME_2, "Consecutive Frame 2"},
    {ISO15765_MESSAGE_TYPES_FR_ACK_FRAME, "Ack Frame"},
    {0, NULL}
};

static const value_string iso15765_flow_status_types[] = {
    {0, "Continue to Send"},
    {1, "Wait"},
    {2, "Overflow"},
    {0, NULL}
};

#define NORMAL_ADDRESSING 1
#define EXTENDED_ADDRESSING 2

#define ZERO_BYTE_ADDRESSING 0
#define ONE_BYTE_ADDRESSING 1
#define TWO_BYTE_ADDRESSING 2

static gint addressing = NORMAL_ADDRESSING;
static gint flexray_addressing = ONE_BYTE_ADDRESSING;
static guint flexray_segment_size_limit = 0;
static guint window = 8;
static range_t *configured_can_ids= NULL;
static range_t *configured_ext_can_ids = NULL;
static gboolean register_lin_diag_frames = TRUE;
static range_t *configured_ipdum_pdu_ids = NULL;
static gint ipdum_addressing = ZERO_BYTE_ADDRESSING;

/* Encoding */
static const enum_val_t enum_addressing[] = {
    {"normal", "Normal addressing", NORMAL_ADDRESSING},
    {"extended", "Extended addressing", EXTENDED_ADDRESSING},
    {NULL, NULL, 0}
};

/* Encoding */
static const enum_val_t enum_flexray_addressing[] = {
    {"1 Byte", "1 byte addressing", ONE_BYTE_ADDRESSING},
    {"2 Byte", "2 byte addressing", TWO_BYTE_ADDRESSING},
    {NULL, NULL, 0}
};

static const enum_val_t enum_ipdum_addressing[] = {
    {"0 Byte", "0 byte addressing", ZERO_BYTE_ADDRESSING},
    {"1 Byte", "1 byte addressing", ONE_BYTE_ADDRESSING},
    {"2 Byte", "2 byte addressing", TWO_BYTE_ADDRESSING},
    {NULL, NULL, 0}
};

static int hf_iso15765_address;
static int hf_iso15765_target_address;
static int hf_iso15765_source_address;
static int hf_iso15765_message_type;
static int hf_iso15765_data_length;
static int hf_iso15765_frame_length;
static int hf_iso15765_sequence_number;
static int hf_iso15765_flow_status;

static int hf_iso15765_fc_bs;
static int hf_iso15765_fc_stmin;
static int hf_iso15765_fc_stmin_in_us;

static int hf_iso15765_autosar_ack;

static gint ett_iso15765;

static expert_field ei_iso15765_message_type_bad;

static int proto_iso15765;
static dissector_handle_t iso15765_handle_can = NULL;
static dissector_handle_t iso15765_handle_lin = NULL;
static dissector_handle_t iso15765_handle_flexray = NULL;
static dissector_handle_t iso15765_handle_ipdum = NULL;
static dissector_handle_t iso15765_handle_pdu_transport = NULL;

static dissector_table_t subdissector_table;

static reassembly_table iso15765_reassembly_table;
static wmem_map_t *iso15765_frame_table = NULL;

static int hf_iso15765_fragments;
static int hf_iso15765_fragment;
static int hf_iso15765_fragment_overlap;
static int hf_iso15765_fragment_overlap_conflicts;
static int hf_iso15765_fragment_multiple_tails;
static int hf_iso15765_fragment_too_long_fragment;
static int hf_iso15765_fragment_error;
static int hf_iso15765_fragment_count;
static int hf_iso15765_reassembled_in;
static int hf_iso15765_reassembled_length;

static gint ett_iso15765_fragment;
static gint ett_iso15765_fragments;

static const fragment_items iso15765_frag_items = {
    /* Fragment subtrees */
    &ett_iso15765_fragment,
    &ett_iso15765_fragments,
    /* Fragment fields */
    &hf_iso15765_fragments,
    &hf_iso15765_fragment,
    &hf_iso15765_fragment_overlap,
    &hf_iso15765_fragment_overlap_conflicts,
    &hf_iso15765_fragment_multiple_tails,
    &hf_iso15765_fragment_too_long_fragment,
    &hf_iso15765_fragment_error,
    &hf_iso15765_fragment_count,
    /* Reassembled in field */
    &hf_iso15765_reassembled_in,
    /* Reassembled length field */
    &hf_iso15765_reassembled_length,
    /* Reassembled data field */
    NULL,
    "ISO15765 fragments"
};

/* UAT for address encoded into CAN IDs */
typedef struct config_can_addr_mapping {
    gboolean extended_address;
    guint32  can_id;
    guint32  can_id_mask;
    guint32  source_addr_mask;
    guint32  target_addr_mask;
    guint32  ecu_addr_mask;
} config_can_addr_mapping_t;

static config_can_addr_mapping_t *config_can_addr_mappings = NULL;
static guint config_can_addr_mappings_num = 0;
#define DATAFILE_CAN_ADDR_MAPPING "ISO15765_can_id_mappings"

UAT_BOOL_CB_DEF(config_can_addr_mappings, extended_address, config_can_addr_mapping_t)
UAT_HEX_CB_DEF(config_can_addr_mappings, can_id, config_can_addr_mapping_t)
UAT_HEX_CB_DEF(config_can_addr_mappings, can_id_mask, config_can_addr_mapping_t)
UAT_HEX_CB_DEF(config_can_addr_mappings, source_addr_mask, config_can_addr_mapping_t)
UAT_HEX_CB_DEF(config_can_addr_mappings, target_addr_mask, config_can_addr_mapping_t)
UAT_HEX_CB_DEF(config_can_addr_mappings, ecu_addr_mask, config_can_addr_mapping_t)

static void *
copy_config_can_addr_mapping_cb(void *n, const void *o, size_t size _U_) {
    config_can_addr_mapping_t *new_rec = (config_can_addr_mapping_t *)n;
    const config_can_addr_mapping_t *old_rec = (const config_can_addr_mapping_t *)o;

    new_rec->extended_address = old_rec->extended_address;
    new_rec->can_id = old_rec->can_id;
    new_rec->can_id_mask = old_rec->can_id_mask;
    new_rec->source_addr_mask = old_rec->source_addr_mask;
    new_rec->target_addr_mask = old_rec->target_addr_mask;
    new_rec->ecu_addr_mask = old_rec->ecu_addr_mask;

    return new_rec;
}

static bool
update_config_can_addr_mappings(void *r, char **err) {
    config_can_addr_mapping_t *rec = (config_can_addr_mapping_t *)r;

    if (rec->source_addr_mask == 0 && rec->target_addr_mask == 0 && rec->ecu_addr_mask == 0) {
        *err = ws_strdup_printf("You need to define the ECU Mask OR Source Mask/Target Mask!");
        return FALSE;
    }

    if ((rec->source_addr_mask != 0 || rec->target_addr_mask != 0) && rec->ecu_addr_mask != 0) {
        *err = ws_strdup_printf("You can only use Source Address Mask/Target Address Mask OR ECU Address Mask! Not both at the same time!");
        return FALSE;
    }

    if ((rec->source_addr_mask == 0 || rec->target_addr_mask == 0) && rec->ecu_addr_mask == 0) {
        *err = ws_strdup_printf("You can only use Source Address Mask and Target Address Mask in combination!");
        return FALSE;
    }

    if (rec->extended_address) {
        if ((rec->source_addr_mask & ~CAN_EFF_MASK) != 0) {
            *err = ws_strdup_printf("Source Address Mask covering bits not allowed for extended IDs (29bit)!");
            return FALSE;
        }
        if ((rec->target_addr_mask & ~CAN_EFF_MASK) != 0) {
            *err = ws_strdup_printf("Target Address Mask covering bits not allowed for extended IDs (29bit)!");
            return FALSE;
        }
        if ((rec->ecu_addr_mask & ~CAN_EFF_MASK) != 0) {
            *err = ws_strdup_printf("ECU Address Mask covering bits not allowed for extended IDs (29bit)!");
            return FALSE;
        }
    } else {
        if ((rec->source_addr_mask & ~CAN_SFF_MASK) != 0) {
            *err = ws_strdup_printf("Source Address Mask covering bits not allowed for standard IDs (11bit)!");
            return FALSE;
        }
        if ((rec->target_addr_mask & ~CAN_SFF_MASK) != 0) {
            *err = ws_strdup_printf("Target Address Mask covering bits not allowed for standard IDs (11bit)!");
            return FALSE;
        }
        if ((rec->ecu_addr_mask & ~CAN_SFF_MASK) != 0) {
            *err = ws_strdup_printf("ECU Address Mask covering bits not allowed for standard IDs (11bit)!");
            return FALSE;
        }
    }

    return TRUE;
}

static void
free_config_can_addr_mappings(void *r _U_) {
    /* do nothing right now */
}

static void
post_update_config_can_addr_mappings_cb(void) {
    /* do nothing right now */
}

static guint16
masked_guint16_value(const guint16 value, const guint16 mask) {
    return (value & mask) >> ws_ctz(mask);
}

static guint32
masked_guint32_value(const guint32 value, const guint32 mask) {
    return (value & mask) >> ws_ctz(mask);
}

/*
 * setting addresses to 0xffffffff, if not found or configured
 * returning number of addresses (0: none, 1:ecu (both addr same), 2:source+target)
 */
static guint8
find_config_can_addr_mapping(gboolean ext_id, guint32 can_id, guint32 *source_addr, guint32 *target_addr) {
    config_can_addr_mapping_t *tmp = NULL;
    guint32 i;

    if (source_addr == NULL || target_addr == NULL || config_can_addr_mappings == NULL) {
        return 0;
    }

    for (i = 0; i < config_can_addr_mappings_num; i++) {
        if (config_can_addr_mappings[i].extended_address == ext_id &&
            (config_can_addr_mappings[i].can_id & config_can_addr_mappings[i].can_id_mask) ==
            (can_id & config_can_addr_mappings[i].can_id_mask)) {
            tmp = &(config_can_addr_mappings[i]);
            break;
        }
    }

    if (tmp != NULL) {
        if (tmp->ecu_addr_mask != 0) {
            *source_addr = masked_guint32_value(can_id, tmp->ecu_addr_mask);
            *target_addr = *source_addr;
            return 1;
        }
        if (tmp->source_addr_mask != 0 && tmp->target_addr_mask != 0) {
            *source_addr = masked_guint32_value(can_id, tmp->source_addr_mask);
            *target_addr = masked_guint32_value(can_id, tmp->target_addr_mask);
            return 2;
        }
    }

    return 0;
}


/* UAT for PDU Transport config */
typedef struct config_pdu_tranport_config {
    guint32 pdu_id;
    guint32 source_address_size;
    guint32 source_address_fixed;
    guint32 target_address_size;
    guint32 target_address_fixed;
    guint32 ecu_address_size;
    guint32 ecu_address_fixed;
} config_pdu_transport_config_t;

static config_pdu_transport_config_t *config_pdu_transport_config_items = NULL;
static guint config_pdu_transport_config_items_num = 0;
#define DATAFILE_PDU_TRANSPORT_CONFIG "ISO15765_pdu_transport_config"

UAT_HEX_CB_DEF(config_pdu_transport_config_items, pdu_id, config_pdu_transport_config_t)
UAT_DEC_CB_DEF(config_pdu_transport_config_items, source_address_size, config_pdu_transport_config_t)
UAT_HEX_CB_DEF(config_pdu_transport_config_items, source_address_fixed, config_pdu_transport_config_t)
UAT_DEC_CB_DEF(config_pdu_transport_config_items, target_address_size, config_pdu_transport_config_t)
UAT_HEX_CB_DEF(config_pdu_transport_config_items, target_address_fixed, config_pdu_transport_config_t)
UAT_DEC_CB_DEF(config_pdu_transport_config_items, ecu_address_size, config_pdu_transport_config_t)
UAT_HEX_CB_DEF(config_pdu_transport_config_items, ecu_address_fixed, config_pdu_transport_config_t)


static void *
copy_config_pdu_transport_config_cb(void *n, const void *o, size_t size _U_) {
    config_pdu_transport_config_t *new_rec = (config_pdu_transport_config_t *)n;
    const config_pdu_transport_config_t *old_rec = (const config_pdu_transport_config_t *)o;

    new_rec->pdu_id = old_rec->pdu_id;
    new_rec->source_address_size = old_rec->source_address_size;
    new_rec->source_address_fixed = old_rec->source_address_fixed;
    new_rec->target_address_size = old_rec->target_address_size;
    new_rec->target_address_fixed = old_rec->target_address_fixed;
    new_rec->ecu_address_size = old_rec->ecu_address_size;
    new_rec->ecu_address_fixed = old_rec->ecu_address_fixed;

    return new_rec;
}

static bool
update_config_pdu_transport_config_item(void *r, char **err) {
    config_pdu_transport_config_t *rec = (config_pdu_transport_config_t *)r;

    gboolean source_address_configured = rec->source_address_size != 0 || rec->source_address_fixed != ISO15765_ADDR_INVALID;
    gboolean target_address_configured = rec->target_address_size != 0 || rec->target_address_fixed != ISO15765_ADDR_INVALID;
    gboolean ecu_address_configured = rec->ecu_address_size != 0 || rec->ecu_address_fixed != ISO15765_ADDR_INVALID;

    if (rec->source_address_size != 0 && rec->source_address_fixed != ISO15765_ADDR_INVALID) {
        *err = ws_strdup_printf("You can either set the size of the source address or configure a fixed value!");
        return FALSE;
    }

    if (rec->target_address_size != 0 && rec->target_address_fixed != ISO15765_ADDR_INVALID) {
        *err = ws_strdup_printf("You can either set the size of the target address or configure a fixed value!");
        return FALSE;
    }

    if (rec->ecu_address_size != 0 && rec->ecu_address_fixed != ISO15765_ADDR_INVALID) {
        *err = ws_strdup_printf("You can either set the size of the ecu address or configure a fixed value!");
        return FALSE;
    }

    if (ecu_address_configured && (source_address_configured || target_address_configured)) {
        *err = ws_strdup_printf("You cannot configure an ecu address and a source or target address at the same time!");
        return FALSE;
    }

    if ((source_address_configured && !target_address_configured) || (!source_address_configured && target_address_configured)) {
        *err = ws_strdup_printf("You can only configure source and target address at the same time but not only one of them!");
        return FALSE;
    }

    return TRUE;
}

static void
free_config_pdu_transport_config(void *r _U_) {
    /* do nothing for now */
}

static void
reset_config_pdu_transport_config_cb(void) {
    /* do nothing for now */
}

static void
post_update_config_pdu_transport_config_cb(void) {
    dissector_delete_all("pdu_transport.id", iso15765_handle_pdu_transport);

    config_pdu_transport_config_t *tmp;
    guint i;
    for (i = 0; i < config_pdu_transport_config_items_num; i++) {
        tmp = &(config_pdu_transport_config_items[i]);
        dissector_add_uint("pdu_transport.id", tmp->pdu_id, iso15765_handle_pdu_transport);
    }
}

static config_pdu_transport_config_t *
find_pdu_transport_config(guint32 pdu_id) {
    guint i;
    for (i = 0; i < config_pdu_transport_config_items_num; i++) {
        if (config_pdu_transport_config_items[i].pdu_id == pdu_id) {
            return &(config_pdu_transport_config_items[i]);
        }
    }

    return NULL;
}

static int
handle_pdu_transport_addresses(tvbuff_t *tvb, packet_info *pinfo _U_, proto_tree *tree, gint offset_orig, guint32 pdu_id, iso15765_info_t *iso15765data) {
    gint offset = offset_orig;
    config_pdu_transport_config_t *config = find_pdu_transport_config(pdu_id);

    iso15765data->number_of_addresses_valid = 0;
    iso15765data->source_address = 0xffffffff;
    iso15765data->target_address = 0xffffffff;

    if (config == NULL) {
        return offset - offset_orig;
    }

    guint32 tmp;
    if (config->ecu_address_size != 0) {
        proto_tree_add_item_ret_uint(tree, hf_iso15765_address, tvb, offset, config->ecu_address_size, ENC_BIG_ENDIAN, &tmp);
        offset += config->ecu_address_size;
        iso15765data->number_of_addresses_valid = 1;
        iso15765data->source_address = tmp;
        iso15765data->target_address = tmp;
        return offset - offset_orig;
    }

    if (config->ecu_address_fixed != ISO15765_ADDR_INVALID) {
        iso15765data->number_of_addresses_valid = 1;
        iso15765data->source_address = config->ecu_address_fixed;
        iso15765data->target_address = config->ecu_address_fixed;
        return offset - offset_orig;
    }

    if (config->source_address_size == 0 && config->source_address_fixed == ISO15765_ADDR_INVALID && config->target_address_size == 0 && config->target_address_fixed == ISO15765_ADDR_INVALID) {
        return offset - offset_orig;
    }

    /* now we can only have two addresses! */
    iso15765data->number_of_addresses_valid = 2;

    if (config->source_address_size != 0) {
        proto_tree_add_item_ret_uint(tree, hf_iso15765_source_address, tvb, offset, config->source_address_size, ENC_BIG_ENDIAN, &tmp);
        offset += config->source_address_size;
        iso15765data->source_address = tmp;
    } else if (config->source_address_fixed != ISO15765_ADDR_INVALID) {
        iso15765data->source_address = config->source_address_fixed;
    }

    if (config->target_address_size != 0) {
        proto_tree_add_item_ret_uint(tree, hf_iso15765_target_address, tvb, offset, config->target_address_size, ENC_BIG_ENDIAN, &tmp);
        offset += config->target_address_size;
        iso15765data->target_address = tmp;
    } else if (config->target_address_fixed != ISO15765_ADDR_INVALID) {
        iso15765data->target_address = config->target_address_fixed;
    }

    return offset - offset_orig;
}

static int
dissect_iso15765(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, guint32 bus_type, guint32 frame_id, guint32 frame_length) {
    static guint32 msg_seqid = 0;

    proto_tree *iso15765_tree;
    proto_item *ti;
    proto_item *message_type_item;
    tvbuff_t*   next_tvb = NULL;
    guint16     pci, message_type;
    iso15765_identifier_t* iso15765_info;
    /* LIN is always extended addressing */
    guint8      ae = (addressing == NORMAL_ADDRESSING && bus_type != ISO15765_TYPE_LIN) ? 0 : 1;
    guint16     frag_id_low = 0;
    guint32     offset;
    gint32      data_length;
    guint32     full_len;
    gboolean    fragmented = FALSE;
    gboolean    complete = FALSE;

    iso15765_info_t iso15765data;

    col_set_str(pinfo->cinfo, COL_PROTOCOL, "ISO15765");
    col_clear(pinfo->cinfo, COL_INFO);

    iso15765_info = (iso15765_identifier_t *)p_get_proto_data(wmem_file_scope(), pinfo, proto_iso15765, 0);

    if (!iso15765_info) {
        iso15765_info = wmem_new0(wmem_file_scope(), iso15765_identifier_t);
        iso15765_info->id = frame_id;
        iso15765_info->last = FALSE;
        p_add_proto_data(wmem_file_scope(), pinfo, proto_iso15765, 0, iso15765_info);
    }

    ti = proto_tree_add_item(tree, proto_iso15765, tvb, 0, -1, ENC_NA);
    iso15765_tree = proto_item_add_subtree(ti, ett_iso15765);

    iso15765data.bus_type = bus_type;
    iso15765data.id = frame_id;
    iso15765data.number_of_addresses_valid = 0;

    if (bus_type == ISO15765_TYPE_FLEXRAY) {
        guint32 tmp;
        proto_tree_add_item_ret_uint(iso15765_tree, hf_iso15765_source_address, tvb, 0, flexray_addressing, ENC_BIG_ENDIAN, &tmp);
        iso15765data.source_address = (guint16)tmp;
        proto_tree_add_item_ret_uint(iso15765_tree, hf_iso15765_target_address, tvb, flexray_addressing, flexray_addressing, ENC_BIG_ENDIAN, &tmp);
        iso15765data.target_address = (guint16)tmp;
        iso15765data.number_of_addresses_valid = 2;
        ae = 2 * flexray_addressing;
    } else if (bus_type == ISO15765_TYPE_IPDUM && ipdum_addressing > 0) {
        guint32 tmp;
        proto_tree_add_item_ret_uint(iso15765_tree, hf_iso15765_source_address, tvb, 0, ipdum_addressing, ENC_BIG_ENDIAN, &tmp);
        iso15765data.source_address = (guint16)tmp;
        proto_tree_add_item_ret_uint(iso15765_tree, hf_iso15765_target_address, tvb, ipdum_addressing, ipdum_addressing, ENC_BIG_ENDIAN, &tmp);
        iso15765data.target_address = (guint16)tmp;
        iso15765data.number_of_addresses_valid = 2;
        ae = 2 * ipdum_addressing;
    } else if (bus_type == ISO15765_TYPE_PDU_TRANSPORT) {
        ae = handle_pdu_transport_addresses(tvb, pinfo, iso15765_tree, 0, frame_id, &iso15765data);
    } else {
        if (ae != 0) {
            guint32 tmp;
            iso15765data.number_of_addresses_valid = 1;
            proto_tree_add_item_ret_uint(iso15765_tree, hf_iso15765_address, tvb, 0, ae, ENC_NA, &tmp);
            iso15765data.source_address = (guint16)tmp;
            iso15765data.target_address = (guint16)tmp;
        } else {
            /* Address implicitly encoded? */
            if (bus_type == ISO15765_TYPE_CAN || bus_type == ISO15765_TYPE_CAN_FD) {
                gboolean ext_id = (CAN_EFF_FLAG & frame_id) == CAN_EFF_FLAG;
                guint32  can_id = ext_id ? frame_id & CAN_EFF_MASK : frame_id & CAN_SFF_MASK;
                iso15765data.number_of_addresses_valid = find_config_can_addr_mapping(ext_id, can_id, &(iso15765data.source_address), &(iso15765data.target_address));
            }
        }
    }

    message_type_item = proto_tree_add_item(iso15765_tree, hf_iso15765_message_type, tvb,
                                            ae, ISO15765_PCI_LEN, ENC_BIG_ENDIAN);

    pci = tvb_get_guint8(tvb, ae);
    message_type = masked_guint16_value(pci, ISO15765_MESSAGE_TYPE_MASK);

    col_add_str(pinfo->cinfo, COL_INFO, val_to_str(message_type, iso15765_message_types, "Unknown (0x%02x)"));

    switch(message_type) {
        case ISO15765_MESSAGE_TYPES_SINGLE_FRAME: {
            if (frame_length > 8 && (pci & ISO15765_MESSAGE_DATA_LENGTH_MASK) == 0) {
                offset = ae + ISO15765_PCI_FD_SF_LEN;
                data_length = tvb_get_guint8(tvb, ae + 1);
                proto_tree_add_item(iso15765_tree, hf_iso15765_data_length, tvb, ae + 1, 1, ENC_BIG_ENDIAN);
            } else {
                offset = ae + ISO15765_PCI_LEN;
                data_length = masked_guint16_value(pci, ISO15765_MESSAGE_DATA_LENGTH_MASK);
                proto_tree_add_uint(iso15765_tree, hf_iso15765_data_length, tvb, ae, 1, data_length);
            }

            next_tvb = tvb_new_subset_length(tvb, offset, data_length);
            complete = TRUE;

            col_append_fstr(pinfo->cinfo, COL_INFO, "(Len: %d)", data_length);
            break;
        }
        case ISO15765_MESSAGE_TYPES_FIRST_FRAME: {
            pci = tvb_get_guint16(tvb, ae, ENC_BIG_ENDIAN);
            if (pci == 0x1000) {
                full_len = tvb_get_guint32(tvb, ae + 2, ENC_BIG_ENDIAN);
                proto_tree_add_item(iso15765_tree, hf_iso15765_frame_length, tvb, ae + 2, 4, ENC_BIG_ENDIAN);
                offset = ae + 2 + 4;
            } else {
                full_len = tvb_get_guint16(tvb, ae, ENC_BIG_ENDIAN) & 0xFFF;
                proto_tree_add_uint(iso15765_tree, hf_iso15765_frame_length, tvb, ae, 2, full_len);
                offset = ae + 2;
            }

            data_length = tvb_reported_length(tvb) - offset;
            if (bus_type == ISO15765_TYPE_FLEXRAY && flexray_segment_size_limit != 0
                && (guint32)data_length > flexray_segment_size_limit - (offset - ae)) {
                data_length = flexray_segment_size_limit - (offset - ae);
            }

            fragmented = TRUE;
            frag_id_low = 0;

            /* Save information */
            if (!(pinfo->fd->visited)) {
                iso15765_frame_t *iso15765_frame = wmem_new0(wmem_file_scope(), iso15765_frame_t);
                iso15765_frame->seq = iso15765_info->seq = ++msg_seqid;
                iso15765_frame->len = full_len;

                wmem_map_insert(iso15765_frame_table, GUINT_TO_POINTER(iso15765_info->seq), iso15765_frame);
            }

            col_append_fstr(pinfo->cinfo, COL_INFO, "(Frame Len: %d)", full_len);
            break;
        }
        case ISO15765_MESSAGE_TYPES_FR_CONSECUTIVE_FRAME_2:
        case ISO15765_MESSAGE_TYPES_CONSECUTIVE_FRAME: {
            offset = ae + ISO15765_PCI_LEN;
            data_length = tvb_reported_length(tvb) - offset;
            frag_id_low = masked_guint16_value(pci, ISO15765_MESSAGE_SEQUENCE_NUMBER_MASK);
            fragmented = TRUE;

            if (bus_type == ISO15765_TYPE_FLEXRAY && flexray_segment_size_limit != 0
                && (guint32)data_length > flexray_segment_size_limit - (offset - ae)) {
                data_length = flexray_segment_size_limit - (offset - ae);
            }

            /* Save information */
            if (!(pinfo->fd->visited)) {
                iso15765_info->seq = msg_seqid;
            }

            proto_tree_add_item(iso15765_tree, hf_iso15765_sequence_number,
                                tvb, ae, ISO15765_PCI_LEN, ENC_BIG_ENDIAN);
            col_append_fstr(pinfo->cinfo, COL_INFO, "(Seq: %d)", (pci & ISO15765_MESSAGE_DATA_LENGTH_MASK));
            break;
        }
        case ISO15765_MESSAGE_TYPES_FR_ACK_FRAME:
        case ISO15765_MESSAGE_TYPES_FLOW_CONTROL: {
            guint32 status = 0;
            guint32 bs = 0;
            guint32 stmin = 0;
            gboolean stmin_in_us = FALSE;
            data_length = 0;

            proto_tree_add_item_ret_uint(iso15765_tree, hf_iso15765_flow_status, tvb, ae,
                                         ISO15765_PCI_LEN, ENC_BIG_ENDIAN, &status);
            proto_tree_add_item_ret_uint(iso15765_tree, hf_iso15765_fc_bs, tvb, ae + ISO15765_FC_BS_OFFSET,
                                         ISO15765_FC_BS_LEN, ENC_BIG_ENDIAN, &bs);

            stmin = tvb_get_guint8(tvb, ae + ISO15765_FC_STMIN_OFFSET);
            if (stmin >= 0xF1 && stmin <= 0xF9) {
                stmin_in_us = TRUE;
                stmin = (stmin - 0xF0) * 100U;
                proto_tree_add_uint(iso15765_tree, hf_iso15765_fc_stmin_in_us, tvb, ae + ISO15765_FC_STMIN_OFFSET,
                    ISO15765_FC_STMIN_LEN, stmin);
            } else {
                proto_tree_add_uint(iso15765_tree, hf_iso15765_fc_stmin, tvb, ae + ISO15765_FC_STMIN_OFFSET,
                                             ISO15765_FC_STMIN_LEN, stmin);
            }

            if (message_type == ISO15765_MESSAGE_TYPES_FR_ACK_FRAME) {
                guint32 ack = 0;
                guint32 sn = 0;
                offset = ae + ISO15765_FC_STMIN_OFFSET + ISO15765_FC_STMIN_LEN;

                proto_tree_add_item_ret_uint(iso15765_tree, hf_iso15765_autosar_ack, tvb, offset, 1, ENC_BIG_ENDIAN, &ack);
                proto_tree_add_item_ret_uint(iso15765_tree, hf_iso15765_sequence_number, tvb, offset, 1, ENC_BIG_ENDIAN, &sn);

                col_append_fstr(pinfo->cinfo, COL_INFO, "(Status: %d, Block size: 0x%x, Separation time minimum: %d %s, Ack: %d, Seq: %d)",
                                status, bs, stmin, stmin_in_us ? "µs" : "ms", ack, sn);
            } else {
                col_append_fstr(pinfo->cinfo, COL_INFO, "(Status: %d, Block size: 0x%x, Separation time minimum: %d %s)",
                                status, bs, stmin, stmin_in_us ? "µs" : "ms");
            }
            break;
        }
        /* And now the AUTOSAR FlexRay TP Types... */
        case ISO15765_MESSAGE_TYPES_FR_SINGLE_FRAME_EXT: {
            offset = ae + ISO15765_PCI_FD_SF_LEN;
            data_length = tvb_get_guint8(tvb, ae + 1);
            proto_tree_add_item(iso15765_tree, hf_iso15765_data_length, tvb, ae + 1, 1, ENC_BIG_ENDIAN);

            next_tvb = tvb_new_subset_length(tvb, offset, data_length);
            complete = TRUE;

            /* Show some info */
            col_append_fstr(pinfo->cinfo, COL_INFO, "(Len: %d)", data_length);
            break;
        }
        case ISO15765_MESSAGE_TYPES_FR_FIRST_FRAME_EXT: {
            full_len = tvb_get_guint32(tvb, ae + 1, ENC_BIG_ENDIAN);
            proto_tree_add_item(iso15765_tree, hf_iso15765_frame_length, tvb, ae + 1, 4, ENC_BIG_ENDIAN);
            offset = ae + 1 + 4;

            data_length = tvb_reported_length(tvb) - offset;
            if (bus_type == ISO15765_TYPE_FLEXRAY && flexray_segment_size_limit != 0
                && (guint32)data_length > flexray_segment_size_limit - (offset - ae)) {
                data_length = flexray_segment_size_limit - (offset - ae);
            }

            fragmented = TRUE;
            frag_id_low = 0;

            /* Save information */
            if (!(pinfo->fd->visited)) {
                iso15765_frame_t *iso15765_frame = wmem_new0(wmem_file_scope(), iso15765_frame_t);
                iso15765_frame->seq = iso15765_info->seq = ++msg_seqid;
                iso15765_frame->len = full_len;

                wmem_map_insert(iso15765_frame_table, GUINT_TO_POINTER(iso15765_info->seq), iso15765_frame);
            }

            /* Show some info */
            col_append_fstr(pinfo->cinfo, COL_INFO, "(Frame Len: %d)", full_len);
            break;
        }
        default:
            expert_add_info_format(pinfo, message_type_item, &ei_iso15765_message_type_bad,
                                   "Bad Message Type value %u <= 7", message_type);
            return ae;
    }

    /* Show data */
    if (data_length > 0) {
        col_append_fstr(pinfo->cinfo, COL_INFO, "   %s",
                        tvb_bytes_to_str_punct(pinfo->pool, tvb, offset, data_length, ' '));
    }

    if (fragmented) {
        tvbuff_t *new_tvb = NULL;
        iso15765_frame_t *iso15765_frame;
        guint16 frag_id = frag_id_low;
        /* Get frame information */
        iso15765_frame = (iso15765_frame_t *)wmem_map_lookup(iso15765_frame_table,
                                                             GUINT_TO_POINTER(iso15765_info->seq));

        if (iso15765_frame != NULL) {
            if (!(pinfo->fd->visited)) {
                DISSECTOR_ASSERT(frag_id < 16);
                guint16 tmp = iso15765_frame->frag_id_high[frag_id]++;
                /* Make sure that we assert on using more than 4096 (16*255) segments.*/
                DISSECTOR_ASSERT(iso15765_frame->frag_id_high[frag_id] != 0);
                frag_id += tmp * 16;

                /* Save the frag_id for subsequent dissection */
                iso15765_info->frag_id = frag_id;

                /* Check if there is an error in conversation */
                if (iso15765_info->frag_id + window < iso15765_frame->last_frag_id) {
                    /* Error in conversation */
                    iso15765_frame->error = TRUE;
                }
            }

            if (!iso15765_frame->error) {
                gboolean       save_fragmented = pinfo->fragmented;
                guint32        len = data_length;
                fragment_head *frag_msg;

                /* Check if it's the last packet */
                if (!(pinfo->fd->visited)) {
                    /* Update the last_frag_id */
                    if (frag_id > iso15765_frame->last_frag_id) {
                        iso15765_frame->last_frag_id = frag_id;
                    }

                    iso15765_frame->offset += len;
                    if (iso15765_frame->offset >= iso15765_frame->len) {
                        iso15765_info->last = TRUE;
                        iso15765_frame->complete = TRUE;
                        len -= (iso15765_frame->offset - iso15765_frame->len);
                    }
                }
                pinfo->fragmented = TRUE;

                /* Add fragment to fragment table */
                frag_msg = fragment_add_seq_check(&iso15765_reassembly_table, tvb, offset, pinfo, iso15765_info->seq, NULL,
                                                  iso15765_info->frag_id, len, !iso15765_info->last);

                new_tvb = process_reassembled_data(tvb, offset, pinfo, "Reassembled Message", frag_msg,
                                                   &iso15765_frag_items, NULL, iso15765_tree);

                if (frag_msg && frag_msg->reassembled_in != pinfo->num) {
                    col_append_frame_number(pinfo, COL_INFO, " [Reassembled in #%u]",
                                    frag_msg->reassembled_in);
                }

                pinfo->fragmented = save_fragmented;
            }

            if (new_tvb) {
                /* This is a complete TVB to dissect */
                next_tvb = new_tvb;
                complete = TRUE;
            } else {
                next_tvb = tvb_new_subset_length(tvb, offset, data_length);
            }
        }
    }

    if (next_tvb) {
        iso15765data.len = frame_length;

        if (!complete || !dissector_try_payload_new(subdissector_table, next_tvb, pinfo, tree, TRUE, &iso15765data)) {
            call_data_dissector(next_tvb, pinfo, tree);
        }
    }

    return tvb_captured_length(tvb);
}

static int
dissect_iso15765_can(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void* data) {
    can_info_t can_info;

    DISSECTOR_ASSERT(data);
    can_info = *((can_info_t*)data);

    if (can_info.id & (CAN_ERR_FLAG | CAN_RTR_FLAG)) {
        /* Error and RTR frames are not for us. */
        return 0;
    }

    if (can_info.fd) {
        return dissect_iso15765(tvb, pinfo, tree, ISO15765_TYPE_CAN_FD, can_info.id, can_info.len);
    } else {
        return dissect_iso15765(tvb, pinfo, tree, ISO15765_TYPE_CAN, can_info.id, can_info.len);
    }
}

static int
dissect_iso15765_lin(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void* data) {
    DISSECTOR_ASSERT(data);

    lin_info_t *lininfo = (lin_info_t *)data;

    return dissect_iso15765(tvb, pinfo, tree, ISO15765_TYPE_LIN, lininfo->id, lininfo->len);
}

static int
dissect_iso15765_flexray(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void* data) {
    DISSECTOR_ASSERT(data);

    flexray_info_t *flexray_id = (flexray_info_t *)data;

    guint32 id = (((guint32)flexray_id->id) << 16) | (((guint32)flexray_id->cc) << 8) | flexray_id->ch;

    return dissect_iso15765(tvb, pinfo, tree, ISO15765_TYPE_FLEXRAY, id, tvb_captured_length(tvb));
}

static int
dissect_iso15765_ipdum(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void* data) {
    DISSECTOR_ASSERT(data);

    autosar_ipdu_multiplexer_info_t *ipdum_data = (autosar_ipdu_multiplexer_info_t *)data;

    return dissect_iso15765(tvb, pinfo, tree, ISO15765_TYPE_IPDUM, ipdum_data->pdu_id, tvb_captured_length(tvb));
}

static int
dissect_iso15765_pdu_transport(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void* data) {
    DISSECTOR_ASSERT(data);

    pdu_transport_info_t *pdu_transport_data = (pdu_transport_info_t *)data;

    return dissect_iso15765(tvb, pinfo, tree, ISO15765_TYPE_PDU_TRANSPORT, pdu_transport_data->id, tvb_captured_length(tvb));
}

static void
update_config(void) {
    if (iso15765_handle_lin != NULL) {
        dissector_delete_all("lin.frame_id", iso15765_handle_lin);
        if (register_lin_diag_frames) {
            /* LIN specification states that 0x3c and 0x3d are for diagnostics */
            dissector_add_uint("lin.frame_id", LIN_DIAG_MASTER_REQUEST_FRAME, iso15765_handle_lin);
            dissector_add_uint("lin.frame_id", LIN_DIAG_SLAVE_RESPONSE_FRAME, iso15765_handle_lin);
        }
    }

    if (iso15765_handle_can != NULL) {
        dissector_delete_all("can.id", iso15765_handle_can);
        dissector_delete_all("can.extended_id", iso15765_handle_can);
        dissector_add_uint_range("can.id", configured_can_ids, iso15765_handle_can);
        dissector_add_uint_range("can.extended_id", configured_ext_can_ids, iso15765_handle_can);
    }

    if (iso15765_handle_ipdum != NULL) {
        dissector_delete_all("ipdum.pdu.id", iso15765_handle_ipdum);
        dissector_add_uint_range("ipdum.pdu.id", configured_ipdum_pdu_ids, iso15765_handle_ipdum);
    }
}

void
proto_register_iso15765(void) {
    uat_t *config_can_addr_mapping_uat;
    uat_t *config_pdu_transport_config_uat;

    static hf_register_info hf[] = {
        { &hf_iso15765_address, {
            "Address", "iso15765.address", FT_UINT8,  BASE_HEX, NULL, 0, NULL, HFILL } },
        { &hf_iso15765_target_address, {
            "Target Address", "iso15765.target_address", FT_UINT16,  BASE_HEX, NULL, 0, NULL, HFILL } },
        { &hf_iso15765_source_address, {
            "Source Address", "iso15765.source_address", FT_UINT16,  BASE_HEX, NULL, 0, NULL, HFILL } },
        { &hf_iso15765_message_type, {
            "Message Type", "iso15765.message_type", FT_UINT8,  BASE_HEX, VALS(iso15765_message_types), ISO15765_MESSAGE_TYPE_MASK, NULL, HFILL } },
        { &hf_iso15765_data_length, {
            "Data length", "iso15765.data_length", FT_UINT32,  BASE_DEC, NULL, 0, NULL, HFILL } },
        { &hf_iso15765_frame_length, {
            "Frame length", "iso15765.frame_length", FT_UINT32,  BASE_DEC, NULL, 0x0, NULL, HFILL } },
        { &hf_iso15765_sequence_number, {
            "Sequence number", "iso15765.sequence_number", FT_UINT8,  BASE_HEX, NULL, ISO15765_MESSAGE_SEQUENCE_NUMBER_MASK, NULL, HFILL } },
        { &hf_iso15765_flow_status, {
            "Flow status", "iso15765.flow_status", FT_UINT8,  BASE_HEX, VALS(iso15765_flow_status_types), ISO15765_MESSAGE_FLOW_STATUS_MASK, NULL, HFILL } },

        { &hf_iso15765_fc_bs, {
            "Block size",    "iso15765.flow_control.bs", FT_UINT8, BASE_HEX, NULL, 0x00, NULL, HFILL } },
        { &hf_iso15765_fc_stmin, {
            "Separation time minimum (ms)", "iso15765.flow_control.stmin", FT_UINT8, BASE_DEC, NULL, 0x00, NULL, HFILL } },
        { &hf_iso15765_fc_stmin_in_us, {
            "Separation time minimum (µs)", "iso15765.flow_control.stmin", FT_UINT8, BASE_DEC, NULL, 0x00, NULL, HFILL } },
        { &hf_iso15765_autosar_ack, {
            "Acknowledgment", "iso15765.autosar_ack.ack", FT_UINT8, BASE_HEX, NULL, ISO15765_MESSAGE_AUTOSAR_ACK_MASK, NULL, HFILL } },

        { &hf_iso15765_fragments, {
            "Message fragments", "iso15765.fragments", FT_NONE, BASE_NONE, NULL, 0x00, NULL, HFILL }, },
        { &hf_iso15765_fragment, {
            "Message fragment", "iso15765.fragment", FT_FRAMENUM, BASE_NONE, NULL, 0x00, NULL, HFILL } },
        { &hf_iso15765_fragment_overlap, {
            "Message fragment overlap", "iso15765.fragment.overlap", FT_BOOLEAN, 0, NULL, 0x00, NULL, HFILL } },
        { &hf_iso15765_fragment_overlap_conflicts, {
            "Message fragment overlapping with conflicting data", "iso15765.fragment.overlap.conflicts", FT_BOOLEAN, 0, NULL, 0x00, NULL, HFILL } },
        { &hf_iso15765_fragment_multiple_tails, {
            "Message has multiple tail fragments", "iso15765.fragment.multiple_tails", FT_BOOLEAN, 0, NULL, 0x00, NULL, HFILL } },
        { &hf_iso15765_fragment_too_long_fragment, {
            "Message fragment too long", "iso15765.fragment.too_long_fragment", FT_BOOLEAN, 0, NULL, 0x00, NULL, HFILL } },
        { &hf_iso15765_fragment_error, {
            "Message defragmentation error", "iso15765.fragment.error", FT_FRAMENUM, BASE_NONE, NULL, 0x00, NULL, HFILL } },
        { &hf_iso15765_fragment_count, {
            "Message fragment count", "iso15765.fragment.count", FT_UINT32, BASE_DEC, NULL, 0x00, NULL, HFILL } },
        { &hf_iso15765_reassembled_in, {
            "Reassembled in", "iso15765.reassembled.in", FT_FRAMENUM, BASE_NONE, NULL, 0x00, NULL, HFILL } },
        { &hf_iso15765_reassembled_length, {
            "Reassembled length", "iso15765.reassembled.length", FT_UINT32, BASE_DEC, NULL, 0x00, NULL, HFILL } },
    };

    /* Setup protocol subtree array */
    static gint *ett[] = {
        &ett_iso15765,
        &ett_iso15765_fragment,
        &ett_iso15765_fragments,
    };

    static ei_register_info ei[] = {
        { &ei_iso15765_message_type_bad, {
            "iso15765.message_type.bad", PI_MALFORMED, PI_ERROR, "Bad Message Type value", EXPFILL } },
    };

    module_t *iso15765_module;
    expert_module_t* expert_iso15765;

    proto_iso15765 = proto_register_protocol ( "ISO15765 Protocol", "ISO 15765", "iso15765");
    register_dissector("iso15765", dissect_iso15765_lin, proto_iso15765);
    expert_iso15765 = expert_register_protocol(proto_iso15765);

    proto_register_field_array(proto_iso15765, hf, array_length(hf));
    proto_register_subtree_array(ett, array_length(ett));

    expert_register_field_array(expert_iso15765, ei, array_length(ei));

    iso15765_module = prefs_register_protocol(proto_iso15765, update_config);

    prefs_register_enum_preference(iso15765_module, "addressing",
                                   "Addressing",
                                   "Addressing of ISO 15765. Normal or Extended",
                                   &addressing,
                                   enum_addressing, TRUE);

    prefs_register_uint_preference(iso15765_module, "window",
                                   "Window",
                                   "Window of ISO 15765 fragments",
                                   10, &window);

    prefs_register_static_text_preference(iso15765_module, "empty_can", "", NULL);

    range_convert_str(wmem_epan_scope(), &configured_can_ids, "", 0x7ff);
    prefs_register_range_preference(iso15765_module, "can.ids",
                                    "CAN IDs (standard)",
                                    "ISO15765 bound standard CAN IDs",
                                    &configured_can_ids, 0x7ff);

    range_convert_str(wmem_epan_scope(), &configured_ext_can_ids, "", 0x1fffffff);
    prefs_register_range_preference(iso15765_module, "can.extended_ids",
                                    "CAN IDs (extended)",
                                    "ISO15765 bound extended CAN IDs",
                                    &configured_ext_can_ids, 0x1fffffff);

    /* UATs for config_can_addr_mapping_uat */
    static uat_field_t config_can_addr_mapping_uat_fields[] = {
        UAT_FLD_BOOL(config_can_addr_mappings, extended_address, "Ext Addr (29bit)",    "29bit Addressing (TRUE), 11bit Addressing (FALSE)"),
        UAT_FLD_HEX(config_can_addr_mappings,  can_id,           "CAN ID",              "CAN ID (hex)"),
        UAT_FLD_HEX(config_can_addr_mappings,  can_id_mask,      "CAN ID Mask",         "CAN ID Mask (hex)"),
        UAT_FLD_HEX(config_can_addr_mappings,  source_addr_mask, "Source Addr Mask",    "Bitmask to specify location of Source Address (hex)"),
        UAT_FLD_HEX(config_can_addr_mappings,  target_addr_mask, "Target Addr Mask",    "Bitmask to specify location of Target Address (hex)"),
        UAT_FLD_HEX(config_can_addr_mappings,  ecu_addr_mask,    "ECU Addr Mask",       "Bitmask to specify location of ECU Address (hex)"),
        UAT_END_FIELDS
    };

    config_can_addr_mapping_uat = uat_new("ISO15765 CAN ID Mapping",
        sizeof(config_can_addr_mapping_t),          /* record size           */
        DATAFILE_CAN_ADDR_MAPPING,                  /* filename              */
        TRUE,                                       /* from profile          */
        (void**)&config_can_addr_mappings,          /* data_ptr              */
        &config_can_addr_mappings_num,              /* numitems_ptr          */
        UAT_AFFECTS_DISSECTION,                     /* but not fields        */
        NULL,                                       /* help                  */
        copy_config_can_addr_mapping_cb,            /* copy callback         */
        update_config_can_addr_mappings,            /* update callback       */
        free_config_can_addr_mappings,              /* free callback         */
        post_update_config_can_addr_mappings_cb,    /* post update callback  */
        NULL,                                       /* reset callback        */
        config_can_addr_mapping_uat_fields          /* UAT field definitions */
    );

    prefs_register_uat_preference(iso15765_module, "_iso15765_can_id_mappings", "CAN ID Mappings",
        "A table to define mappings rules for CAN IDs", config_can_addr_mapping_uat);

    prefs_register_static_text_preference(iso15765_module, "empty_lin", "", NULL);
    prefs_register_bool_preference(iso15765_module, "lin_diag",
                                   "Handle LIN Diagnostic Frames",
                                   "Handle LIN Diagnostic Frames",
                                   &register_lin_diag_frames);

    prefs_register_static_text_preference(iso15765_module, "empty_fr", "", NULL);
    prefs_register_enum_preference(iso15765_module, "flexray_addressing",
                                   "FlexRay Addressing",
                                   "Addressing of FlexRay TP. 1 Byte or 2 Byte",
                                   &flexray_addressing,
                                   enum_flexray_addressing, TRUE);

    prefs_register_uint_preference(iso15765_module, "flexray_segment_size_limit",
                                   "FlexRay Segment Cutoff",
                                   "Segment Size Limit for first and consecutive frames of FlexRay (bytes after addresses)",
                                   10, &flexray_segment_size_limit);


    prefs_register_static_text_preference(iso15765_module, "empty_ipdum", "", NULL);
    range_convert_str(wmem_epan_scope(), &configured_ipdum_pdu_ids, "", 0xffffffff);
    prefs_register_range_preference(iso15765_module, "ipdum.pdu.id",
        "I-PduM PDU-IDs",
        "I-PduM PDU-IDs",
        &configured_ipdum_pdu_ids, 0xffffffff);

    prefs_register_enum_preference(iso15765_module, "ipdum_addressing",
        "I-PduM Addressing",
        "Addressing of I-PduM TP. 0, 1, or 2 Bytes",
        &ipdum_addressing,
        enum_ipdum_addressing, TRUE);

    prefs_register_static_text_preference(iso15765_module, "empty_pdu_transport", "", NULL);

    /* UATs for config_pdu_transport_uat */
    static uat_field_t config_pdu_transport_uat_fields[] = {
        UAT_FLD_HEX(config_pdu_transport_config_items, pdu_id,               "PDU ID",             "PDU ID (hex)"),
        UAT_FLD_DEC(config_pdu_transport_config_items, source_address_size,  "Source Addr. Size",  "Size of encoded source address (0, 1, 2 bytes)"),
        UAT_FLD_HEX(config_pdu_transport_config_items, source_address_fixed, "Source Addr. Fixed", "Fixed source address for this PDU ID (hex), 0xffffffff is invalid"),
        UAT_FLD_DEC(config_pdu_transport_config_items, target_address_size,  "Target Addr. Size",  "Size of encoded target address (0, 1, 2 bytes)"),
        UAT_FLD_HEX(config_pdu_transport_config_items, target_address_fixed, "Target Addr. Fixed", "Fixed target address for this PDU ID (hex), 0xffffffff is invalid"),
        UAT_FLD_DEC(config_pdu_transport_config_items, ecu_address_size,     "Single Addr. Size",  "Size of encoded address (0, 1, 2 bytes)"),
        UAT_FLD_HEX(config_pdu_transport_config_items, ecu_address_fixed,    "Single Addr. Fixed", "Fixed address for this PDU ID (hex), 0xffffffff is invalid"),
        UAT_END_FIELDS
    };

    config_pdu_transport_config_uat = uat_new("ISO15765 PDU Transport Config",
        sizeof(config_pdu_transport_config_t),      /* record size           */
        DATAFILE_PDU_TRANSPORT_CONFIG,              /* filename              */
        TRUE,                                       /* from profile          */
        (void**)&config_pdu_transport_config_items, /* data_ptr              */
        &config_pdu_transport_config_items_num,     /* numitems_ptr          */
        UAT_AFFECTS_DISSECTION,                     /* but not fields        */
        NULL,                                       /* help                  */
        copy_config_pdu_transport_config_cb,        /* copy callback         */
        update_config_pdu_transport_config_item,    /* update callback       */
        free_config_pdu_transport_config,           /* free callback         */
        post_update_config_pdu_transport_config_cb, /* post update callback  */
        reset_config_pdu_transport_config_cb,       /* reset callback        */
        config_pdu_transport_uat_fields             /* UAT field definitions */
    );

    prefs_register_uat_preference(iso15765_module, "_iso15765_pdu_transport_config", "PDU Transport Config",
        "A table to define the PDU Transport Config", config_pdu_transport_config_uat);


    iso15765_frame_table = wmem_map_new_autoreset(wmem_epan_scope(), wmem_file_scope(), g_direct_hash, g_direct_equal);

    reassembly_table_register(&iso15765_reassembly_table, &addresses_reassembly_table_functions);

    subdissector_table = register_decode_as_next_proto(proto_iso15765, "iso15765.subdissector", "ISO15765 next level dissector", NULL);
}

void
proto_reg_handoff_iso15765(void) {
    iso15765_handle_can = create_dissector_handle(dissect_iso15765_can, proto_iso15765);
    iso15765_handle_lin = create_dissector_handle(dissect_iso15765_lin, proto_iso15765);
    iso15765_handle_flexray = create_dissector_handle(dissect_iso15765_flexray, proto_iso15765);
    iso15765_handle_ipdum = create_dissector_handle(dissect_iso15765_ipdum, proto_iso15765);
    iso15765_handle_pdu_transport = create_dissector_handle(dissect_iso15765_pdu_transport, proto_iso15765);
    dissector_add_for_decode_as("can.subdissector", iso15765_handle_can);
    dissector_add_for_decode_as("flexray.subdissector", iso15765_handle_flexray);
    update_config();
}

/*
 * Editor modelines  -  https://www.wireshark.org/tools/modelines.html
 *
 * Local variables:
 * c-basic-offset: 4
 * tab-width: 8
 * indent-tabs-mode: nil
 * End:
 *
 * vi: set shiftwidth=4 tabstop=8 expandtab:
 * :indentSize=4:tabSize=8:noTabs=true:
 */
