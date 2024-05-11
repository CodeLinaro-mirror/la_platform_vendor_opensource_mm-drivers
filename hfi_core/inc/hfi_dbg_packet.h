// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _HFI_CORE_DEBUG_PACKET_H_
#define _HFI_CORE_DEBUG_PACKET_H_

#if IS_ENABLED(CONFIG_DEBUG_FS)

#define HFI_ERROR                                                      1
#define HFI_HEADER_CMD_BUFF_TYPE_START_BIT                            24
#define HFI_PACKET_PAYLOAD_TYPE_START_BIT                             21
#define HFI_COMMAND_DEBUG_LOOPBACK_U32                        0xFF000002
/* 24 bits long */
#define HFI_HEADER_SIZE_MAX     \
	((1 << HFI_HEADER_CMD_BUFF_TYPE_START_BIT) - 1)
/* 20 bits long */
#define HFI_PACKET_SIZE_MAX     \
	((1 << HFI_PACKET_PAYLOAD_TYPE_START_BIT) - 1)

#define GET_HEADER_SIZE(data)           \
	(data & ((1 << HFI_HEADER_CMD_BUFF_TYPE_START_BIT) - 1))
#define GET_HEADER_CMD_BUFF_TYPE(data)         \
	(data >> HFI_HEADER_CMD_BUFF_TYPE_START_BIT)
#define GET_PACKET_SIZE(data)           \
	(data & ((1 << HFI_PACKET_PAYLOAD_TYPE_START_BIT) - 1))
#define GET_PACKET_PAYLOAD_TYPE(data)   \
	(data >> HFI_PACKET_PAYLOAD_TYPE_START_BIT)

#define HFI_PACK_KEY(property_id, version, dsize)					  \
	(property_id | (version << 20) | (dsize << 24))

#define HFI_UNPACK_KEY(key, property_id, version, size)         \
	do {                                                    \
		property_id = (key & 0x000FFFFF);               \
		version     = ((key & 0x00F00000) >> 20);       \
		size        = ((key & 0xFF000000) >> 24);       \
	} while (0)

enum hfi_packet_payload_type {
	HFI_PAYLOAD_NONE                = 0x0,
	HFI_PAYLOAD_U32                 = 0x1,
	HFI_PAYLOAD_U32_ARRAY           = 0x2,
	HFI_PAYLOAD_U64                 = 0x3,
	HFI_PAYLOAD_U64_ARRAY           = 0x4,
	HFI_PAYLOAD_BLOB                = 0x5,
};

enum hfi_cmd_buff_type {
	HFI_CMD_BUFF_SYSTEM          = 0x1,
	HFI_CMD_BUFF_DEVICE          = 0x2,
	HFI_CMD_BUFF_DISPLAY         = 0x3,
	HFI_CMD_BUFF_DEBUG           = 0x4,
	HFI_CMD_BUFF_VIRTUALIZATION  = 0x5,
};

enum hfi_packet_tx_flags {
	HFI_TX_FLAGS_NONE              = 0x0,
	HFI_TX_FLAGS_INTR_REQUIRED     = 0x1,
	HFI_TX_FLAGS_RESPONSE_REQUIRED = 0x2,
	HFI_TX_FLAGS_NON_DISCARDABLE   = 0x4,
};

enum hfi_packet_rx_flags {
	HFI_RX_FLAGS_NONE               = 0x0,
	HFI_RX_FLAGS_SUCCESS            = 0x1,
	HFI_RX_FLAGS_INFORMATION        = 0x2,
	HFI_RX_FLAGS_DEVICE_ERROR       = 0x4,
	HFI_RX_FLAGS_SYSTEM_ERROR       = 0x8,
};

struct hfi_cmd_buff_hdl {
	void *cmd_buffer;
	u32 size;
};

struct hfi_header_info {
	u32 num_packets;
	enum hfi_cmd_buff_type cmd_buff_type;
	u32 object_id;
	u32 header_id;
};

struct hfi_packet_info {
	u32 cmd;
	u32 id;
	u32 flags;
	u32 packet_id;
	enum hfi_packet_payload_type payload_type;
	u32 payload_size;
	void *payload_ptr;
};

struct hfi_header {
	u32 cmd_buff_info;
	u16 device_id;
	u16 object_id;
	u32 timestamp_hi;
	u32 timestamp_lo;
	u32 header_id;
	u32 reserved[2];
	u32 num_packets;
};

struct hfi_packet {
	u32 payload_info;
	u32 cmd;
	u32 flags;
	u32 id;
	u32 packet_id;
	u32 reserved[3];
};

int hfi_create_header(struct hfi_cmd_buff_hdl *cmd_buf_hdl,
	struct hfi_header_info *header_info);

int hfi_create_full_packet(struct hfi_cmd_buff_hdl *cmd_buf_hdl,
	struct hfi_packet_info *packet_info);

int hfi_unpacker_get_header_info(struct hfi_cmd_buff_hdl *cmd_buf_hdl,
	struct hfi_header_info *header_info);

int hfi_unpacker_get_packet_info(struct hfi_cmd_buff_hdl *cmd_buf_hdl,
	u32 packet_num, struct hfi_packet_info *packet_info);

#endif // CONFIG_DEBUG_FS

#endif // _HFI_CORE_DEBUG_PACKET_H_