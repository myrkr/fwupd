/*
 * Copyright (C) 2020 Benson Leung <bleung@chromium.org>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include <string.h>

#include "fu-chunk.h"
#include "fu-cros-ec-usb-device.h"
#include "fu-cros-ec-common.h"
#include "fu-cros-ec-firmware.h"

#define USB_SUBCLASS_GOOGLE_UPDATE	0x53
#define USB_PROTOCOL_GOOGLE_UPDATE	0xff

#define SETUP_RETRY_CNT			5
#define MAX_BLOCK_XFER_RETRIES		10
#define FLUSH_TIMEOUT_MS		10
#define BULK_SEND_TIMEOUT_MS		2000
#define BULK_RECV_TIMEOUT_MS		5000

#define UPDATE_DONE			0xB007AB1E

struct _FuCrosEcUsbDevice {
	FuUsbDevice			parent_instance;
	guint8				iface_idx; 	/* bInterfaceNumber */
	guint8				ep_num;		/* bEndpointAddress */
	guint16				chunk_len; 	/* wMaxPacketSize */

	struct first_response_pdu	targ;
	guint32				writeable_offset;
	guint16				protocol_version;
	guint16				header_type;
	struct cros_ec_version		version;
};

G_DEFINE_TYPE (FuCrosEcUsbDevice, fu_cros_ec_usb_device, FU_TYPE_USB_DEVICE)

typedef union _START_RESP {
	struct first_response_pdu rpdu;
	guint32 legacy_resp;
} START_RESP;

typedef struct {
	struct update_frame_header ufh;
	GBytes *image_bytes;
	gsize offset;
	gsize payload_size;
} FuCrosEcUsbBlockInfo;

static gboolean
fu_cros_ec_usb_device_find_interface (FuUsbDevice *device,
				      GError **error)
{
	GUsbDevice *usb_device = fu_usb_device_get_dev (device);
	FuCrosEcUsbDevice *self = FU_CROS_EC_USB_DEVICE (device);
	g_autoptr(GPtrArray) intfs = NULL;

	/* based on usb_updater2's find_interfacei() and find_endpoint() */

	intfs = g_usb_device_get_interfaces (usb_device, error);
	if (intfs == NULL)
		return FALSE;
	for (guint i = 0; i < intfs->len; i++) {
		GUsbInterface *intf = g_ptr_array_index (intfs, i);
		if (g_usb_interface_get_class (intf) == 255 &&
		    g_usb_interface_get_subclass (intf) == USB_SUBCLASS_GOOGLE_UPDATE &&
		    g_usb_interface_get_protocol (intf) == USB_PROTOCOL_GOOGLE_UPDATE) {
			GUsbEndpoint *ep;
			g_autoptr(GPtrArray) endpoints = NULL;

			endpoints = g_usb_interface_get_endpoints (intf);
			if (NULL == endpoints || 0 == endpoints->len)
				continue;
			ep = g_ptr_array_index (endpoints, 0);
			self->iface_idx = g_usb_interface_get_number (intf);
			self->ep_num = g_usb_endpoint_get_address (ep) & 0x7f;
			self->chunk_len = g_usb_endpoint_get_maximum_packet_size (ep);

			return TRUE;
		}
	}
	g_set_error_literal (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_FOUND,
			     "no update interface found");
	return FALSE;
}

static gboolean
fu_cros_ec_usb_device_open (FuUsbDevice *device, GError **error)
{
	GUsbDevice *usb_device = fu_usb_device_get_dev (device);
	FuCrosEcUsbDevice *self = FU_CROS_EC_USB_DEVICE (device);

	if (!g_usb_device_claim_interface (usb_device, self->iface_idx,
					   G_USB_DEVICE_CLAIM_INTERFACE_BIND_KERNEL_DRIVER,
					   error)) {
		g_prefix_error (error, "failed to claim interface: ");
		return FALSE;
	}

	/* success */
	return TRUE;
}

static gboolean
fu_cros_ec_usb_device_probe (FuUsbDevice *device, GError **error)
{
	FuCrosEcUsbDevice *self = FU_CROS_EC_USB_DEVICE (device);

	/* very much like usb_updater2's usb_findit() */

	if (!fu_cros_ec_usb_device_find_interface (device, error)) {
		g_prefix_error (error, "failed to find update interface: ");
		return FALSE;
	}

	if (self->chunk_len == 0) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_INVALID_DATA,
			     "wMaxPacketSize isn't valid: %" G_GUINT16_FORMAT,
			     self->chunk_len);
		return FALSE;
	}

	/* success */
	return TRUE;
}

static gboolean
fu_cros_ec_usb_device_do_xfer (FuCrosEcUsbDevice * self, guint8 *outbuf,
			       gsize outlen, guint8 *inbuf, gsize inlen,
			       gboolean allow_less, gsize *rxed_count,
			       GError **error)
{
	GUsbDevice *usb_device = fu_usb_device_get_dev (FU_USB_DEVICE (self));
	gsize actual = 0;

	/* send data out */
	if (outbuf != NULL && outlen > 0) {
		if (!g_usb_device_bulk_transfer (usb_device, self->ep_num,
						 outbuf, outlen,
						 &actual, BULK_SEND_TIMEOUT_MS,
						 NULL, error)) {
			return FALSE;
		}
		if (actual != outlen) {
			g_set_error (error,
				     G_IO_ERROR,
				     G_IO_ERROR_PARTIAL_INPUT,
				     "only sent %" G_GSIZE_FORMAT "/%"
				     G_GSIZE_FORMAT " bytes",
				     actual, outlen);
			return FALSE;
		}
	}

	/* read reply back */
	if (inbuf != NULL && inlen > 0) {
		actual = 0;
		if (!g_usb_device_bulk_transfer (usb_device,
						 self->ep_num | 0x80,
						 inbuf, inlen,
						 &actual, BULK_RECV_TIMEOUT_MS,
						 NULL, error)) {
			return FALSE;
		}
		if (actual != inlen && !allow_less) {
			g_set_error (error,
				     G_IO_ERROR,
				     G_IO_ERROR_PARTIAL_INPUT,
				     "only received %" G_GSIZE_FORMAT "/%"
				     G_GSIZE_FORMAT " bytes",
				     actual, outlen);
			return FALSE;
		}
	}

	if (rxed_count != NULL)
		*rxed_count = actual;

	return TRUE;
}

static gboolean
fu_cros_ec_usb_device_flush (FuDevice *device, gpointer user_data,
			     GError **error)
{
	GUsbDevice *usb_device = fu_usb_device_get_dev (FU_USB_DEVICE (device));
	FuCrosEcUsbDevice *self = FU_CROS_EC_USB_DEVICE (device);
	gsize actual = 0;
	g_autofree guint8 *inbuf = g_malloc0 (self->chunk_len);

	if (g_usb_device_bulk_transfer (usb_device, self->ep_num | 0x80, inbuf,
					self->chunk_len, &actual,
					FLUSH_TIMEOUT_MS, NULL, NULL)) {
		g_debug ("flushing %" G_GSIZE_FORMAT " bytes", actual);
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_FAILED,
			     "flushing %" G_GSIZE_FORMAT " bytes", actual);
		return FALSE;
	}

	/* success */
	return TRUE;
}

static gboolean
fu_cros_ec_usb_device_start_request (FuDevice *device, gpointer user_data,
				     GError **error)
{
	FuCrosEcUsbDevice *self = FU_CROS_EC_USB_DEVICE (device);
	guint8 *start_resp = (guint8 *) user_data;
	struct update_frame_header ufh;
	gsize rxed_size = 0;

	memset(&ufh, 0, sizeof (ufh));
	ufh.block_size = GUINT32_TO_BE (sizeof(ufh));
	if (!fu_cros_ec_usb_device_do_xfer (self, (guint8 *)&ufh, sizeof(ufh),
					    start_resp,
					    sizeof(START_RESP), TRUE,
					    &rxed_size, error))
		return FALSE;

	/* we got something, so check for errors in response */
	if (rxed_size < 8) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_PARTIAL_INPUT,
			     "unexpected response size %" G_GSIZE_FORMAT,
			     rxed_size);
		return FALSE;
	}

	/* success */
	return TRUE;
}

static gboolean
fu_cros_ec_usb_device_setup (FuDevice *device, GError **error)
{
	FuCrosEcUsbDevice *self = FU_CROS_EC_USB_DEVICE (device);
	guint32 error_code;
	START_RESP start_resp;

	/* flush all data from endpoint to recover in case of error */
	if (!fu_device_retry (device, fu_cros_ec_usb_device_flush,
			      SETUP_RETRY_CNT, NULL, error)) {
		g_prefix_error (error, "failed to flush device to idle state: ");
		return FALSE;
	}

	/* send start request */
	if (!fu_device_retry (device, fu_cros_ec_usb_device_start_request,
			      SETUP_RETRY_CNT, &start_resp, error)) {
		g_prefix_error (error, "failed to send start request: ");
		return FALSE;
	}

	self->protocol_version = GUINT16_FROM_BE (start_resp.rpdu.protocol_version);

	if (self->protocol_version < 5 || self->protocol_version > 6) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_NOT_SUPPORTED,
			     "unsupported protocol version %d",
			     self->protocol_version);
		return FALSE;
	}
	self->header_type = GUINT16_FROM_BE (start_resp.rpdu.header_type);

	error_code = GUINT32_FROM_BE (start_resp.rpdu.return_value);
	if (error_code != 0) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_FAILED,
			     "target reporting error %u", error_code);
		return FALSE;
	}

	self->writeable_offset = GUINT32_FROM_BE (start_resp.rpdu.common.offset);
	if (!fu_memcpy_safe ((guint8 *) self->targ.common.version,
			     FU_CROS_EC_STRLEN, 0x0,
			     (const guint8 *) start_resp.rpdu.common.version,
			     sizeof(start_resp.rpdu.common.version), 0x0,
			     sizeof(start_resp.rpdu.common.version), error))
		return FALSE;
	self->targ.common.maximum_pdu_size =
		GUINT32_FROM_BE (start_resp.rpdu.common.maximum_pdu_size);
	self->targ.common.flash_protection =
		GUINT32_FROM_BE (start_resp.rpdu.common.flash_protection);
	self->targ.common.min_rollback = GINT32_FROM_BE (start_resp.rpdu.common.min_rollback);
	self->targ.common.key_version = GUINT32_FROM_BE (start_resp.rpdu.common.key_version);

	if (!fu_cros_ec_parse_version (self->targ.common.version,
				       &self->version, error)) {
		g_prefix_error (error,
				"failed parsing device's version: %32s: ",
				self->targ.common.version);
		return FALSE;
	}

	fu_device_set_version (FU_DEVICE (device), self->version.triplet);
	fu_device_add_instance_id (FU_DEVICE (device), self->version.boardname);

	/* success */
	return TRUE;
}

static gboolean
fu_cros_ec_usb_device_transfer_block (FuDevice *device, gpointer user_data,
				      GError **error)
{
	FuCrosEcUsbDevice *self = FU_CROS_EC_USB_DEVICE (device);
	FuCrosEcUsbBlockInfo *block_info = (FuCrosEcUsbBlockInfo *) user_data;
	gsize image_size = 0;
	gsize transfer_size = 0;
	guint32 reply = 0;
	g_autoptr(GBytes) block_bytes = NULL;
	g_autoptr(GPtrArray) chunks = NULL;

	g_return_val_if_fail (block_info != NULL, FALSE);

	image_size = g_bytes_get_size (block_info->image_bytes);
	if (block_info->offset + block_info->payload_size > image_size) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_INVALID_DATA,
			     "offset %" G_GSIZE_FORMAT "plus payload_size %"
			     G_GSIZE_FORMAT " exceeds image size %"
			     G_GSIZE_FORMAT,
			     block_info->offset, block_info->payload_size,
			     image_size);
		return FALSE;
	}

	block_bytes = g_bytes_new_from_bytes (block_info->image_bytes,
					      block_info->offset,
					      block_info->payload_size);
	chunks = fu_chunk_array_new_from_bytes (block_bytes,
						0x00,
						0x00,
						self->chunk_len);

	/* first send the header */
	if (!fu_cros_ec_usb_device_do_xfer (self, (guint8 *)&block_info->ufh,
					    sizeof(struct update_frame_header),
					    NULL,
					    0, FALSE,
					    NULL, error))
		return FALSE;

	/* send the block, chunk by chunk */
	for (guint i = 0; i < chunks->len; i++) {
		FuChunk *chk = g_ptr_array_index (chunks, i);

		if (!fu_cros_ec_usb_device_do_xfer (self,
						    (guint8 *)chk->data,
						    chk->data_sz,
						    NULL,
						    0, FALSE,
						    NULL, error)) {
			return FALSE;
		}
	}

	/* get the reply */
	if (!fu_cros_ec_usb_device_do_xfer (self, NULL, 0,
					    (guint8 *)&reply, sizeof (reply),
					    TRUE, &transfer_size, error))
		return FALSE;
	if (transfer_size == 0) {
		g_set_error_literal (error,
				     G_IO_ERROR,
				     G_IO_ERROR_FAILED,
				     "zero bytes received for block reply");
		return FALSE;
	}
	if (reply != 0) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_FAILED,
			     "error: status 0x%#x", reply);
		return FALSE;
	}

	/* success */
	return TRUE;
}

static gboolean
fu_cros_ec_usb_device_transfer_section (FuDevice *device,
					FuFirmware *firmware,
					FuCrosEcFirmwareSection *section,
					GError **error)
{
	FuCrosEcUsbDevice *self = FU_CROS_EC_USB_DEVICE (device);
	const guint8 * data_ptr = NULL;
	guint32 section_addr = 0;
	gsize data_len = 0;
	gsize offset = 0;
	g_autoptr(GBytes) img_bytes = NULL;

	g_return_val_if_fail (section != NULL, FALSE);

	section_addr = section->offset;
	img_bytes = fu_firmware_get_image_by_idx_bytes (firmware,
							section->image_idx,
							error);
	if (img_bytes == NULL) {
		g_prefix_error (error, "failed to find section image: ");
		return FALSE;
	}

	data_ptr = (const guint8 *)g_bytes_get_data (img_bytes, &data_len);
	if (data_ptr == NULL || data_len != section->size) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_INVALID_DATA,
			     "image and section sizes do not match: image = %"
			     G_GSIZE_FORMAT " bytes vs section size = %"
			     G_GSIZE_FORMAT " bytes",
			     data_len, section->size);
		return FALSE;
	}

	/* smart update: trim trailing bytes */
	while (data_len != 0 && (data_ptr[data_len - 1] == 0xff))
		data_len--;
	g_debug ("trimmed %" G_GSIZE_FORMAT " trailing bytes",
		 section->size - data_len);

	g_debug ("sending 0x%zx bytes to %#x", data_len, section_addr);
	while (data_len > 0) {
		gsize payload_size;
		guint32 block_base;
		FuCrosEcUsbBlockInfo block_info;

		/* prepare the header to prepend to the block */
		block_info.image_bytes = img_bytes;
		payload_size = MIN (data_len,
				    self->targ.common.maximum_pdu_size);
		block_base = GUINT32_TO_BE (section_addr);
		block_info.ufh.block_size = GUINT32_TO_BE (payload_size +
							   sizeof (struct update_frame_header));
		block_info.ufh.cmd.block_base = block_base;
		block_info.ufh.cmd.block_digest = 0;
		block_info.offset = offset;
		block_info.payload_size = payload_size;

		if (!fu_device_retry (device,
				      fu_cros_ec_usb_device_transfer_block,
				      MAX_BLOCK_XFER_RETRIES, &block_info,
				      error)) {
			g_prefix_error (error,
					"failed to transfer block, %"
					G_GSIZE_FORMAT " to go: ", data_len);
			return FALSE;
		}
		data_len -= payload_size;
		offset += payload_size;
		section_addr += payload_size;
	}

	/* success */
	return TRUE;
}

static void
fu_cros_ec_usb_device_send_done (FuDevice *device)
{
	guint32 out = GUINT32_TO_BE (UPDATE_DONE);
	g_autoptr(GError) error_local = NULL;

	/* send stop request, ignoring reply */
	if (!fu_cros_ec_usb_device_do_xfer (FU_CROS_EC_USB_DEVICE (device),
					    (guint8 *)&out, sizeof (out),
					    (guint8 *)&out, 1,
					    FALSE, NULL, &error_local)) {
		g_debug ("error on transfer of done: %s",
			 error_local->message);
	}
}

static gboolean
fu_cros_ec_usb_device_write_firmware (FuDevice *device,
				      FuFirmware *firmware,
				      FwupdInstallFlags flags,
				      GError **error)
{
	GPtrArray *sections;
	FuCrosEcFirmware *cros_ec_firmware = FU_CROS_EC_FIRMWARE (firmware);
	gint num_txed_sections = 0;

	sections = fu_cros_ec_firmware_get_sections (cros_ec_firmware);
	if (sections == NULL) {
		g_set_error_literal (error,
				     G_IO_ERROR,
				     G_IO_ERROR_INVALID_DATA,
				     "invalid sections");
		return FALSE;
	}

	fu_device_set_status (device, FWUPD_STATUS_DEVICE_WRITE);
	for (guint i = 0; i < sections->len; i++) {
		FuCrosEcFirmwareSection *section = g_ptr_array_index (sections, i);

		if (section->ustatus == FU_CROS_EC_FW_NEEDED) {
			if (!fu_cros_ec_usb_device_transfer_section (device,
								     firmware,
								     section,
								     error)) {
				return FALSE;
			}
			num_txed_sections++;
		}
	}
	/* send done */
	fu_cros_ec_usb_device_send_done (device);

	if (num_txed_sections == 0) {
		g_set_error_literal (error,
				     G_IO_ERROR,
				     G_IO_ERROR_INVALID_DATA,
				     "no sections transferred");
		return FALSE;
	}

	/* success */
	return TRUE;
}

static gboolean
fu_cros_ec_usb_device_close (FuUsbDevice *device, GError **error)
{
	GUsbDevice *usb_device = fu_usb_device_get_dev (device);
	FuCrosEcUsbDevice *self = FU_CROS_EC_USB_DEVICE (device);

	if (!g_usb_device_release_interface (usb_device, self->iface_idx,
					     G_USB_DEVICE_CLAIM_INTERFACE_BIND_KERNEL_DRIVER,
					     error)) {
		g_prefix_error (error, "failed to release interface: ");
		return FALSE;
	}

	/* success */
	return TRUE;
}

static FuFirmware *
fu_cros_ec_usb_device_prepare_firmware (FuDevice *device,
					GBytes *fw,
					FwupdInstallFlags flags,
					GError **error)
{
	FuCrosEcUsbDevice *self = FU_CROS_EC_USB_DEVICE (device);
	FuCrosEcFirmware *cros_ec_firmware = NULL;
	g_autoptr(FuFirmware) firmware = fu_cros_ec_firmware_new ();

	fu_device_set_status (device, FWUPD_STATUS_DECOMPRESSING);
	if (!fu_firmware_parse (firmware, fw, flags, error))
		return NULL;
	cros_ec_firmware = FU_CROS_EC_FIRMWARE (firmware);

	/* pick sections */
	if (!fu_cros_ec_firmware_pick_sections (cros_ec_firmware,
						self->writeable_offset,
						error)) {
		g_prefix_error (error, "failed to pick sections: ");
		return NULL;
	}
	return g_steal_pointer (&firmware);
}

static void
fu_cros_ec_usb_device_init (FuCrosEcUsbDevice *device)
{
	fu_device_set_protocol (FU_DEVICE (device), "com.google.usb.crosec");
	fu_device_set_version_format (FU_DEVICE (device), FWUPD_VERSION_FORMAT_TRIPLET);
}

static void
fu_cros_ec_usb_device_to_string (FuDevice *device, guint idt, GString *str)
{
	FuCrosEcUsbDevice *self = FU_CROS_EC_USB_DEVICE (device);
	g_autofree gchar *min_rollback = NULL;

	fu_common_string_append_kv (str, idt, "GitHash", self->version.sha1);
	fu_common_string_append_kb (str, idt, "Dirty",
				    self->version.dirty);
	fu_common_string_append_ku (str, idt, "ProtocolVersion",
				    self->protocol_version);
	fu_common_string_append_ku (str, idt, "HeaderType",
				    self->header_type);
	fu_common_string_append_ku (str, idt, "MaxPDUSize",
				    self->targ.common.maximum_pdu_size);
	fu_common_string_append_kx (str, idt, "FlashProtectionStatus",
				    self->targ.common.flash_protection);
	fu_common_string_append_kv (str, idt, "RawVersion",
				    self->targ.common.version);
	fu_common_string_append_ku (str, idt, "KeyVersion",
				    self->targ.common.key_version);
	min_rollback = g_strdup_printf ("%" G_GINT32_FORMAT,
					self->targ.common.min_rollback);
	fu_common_string_append_kv (str, idt, "MinRollback", min_rollback);
	fu_common_string_append_kx (str, idt, "WriteableOffset",
				    self->writeable_offset);
}

static void
fu_cros_ec_usb_device_class_init (FuCrosEcUsbDeviceClass *klass)
{
	FuDeviceClass *klass_device = FU_DEVICE_CLASS (klass);
	FuUsbDeviceClass *klass_usb_device = FU_USB_DEVICE_CLASS (klass);
	klass_device->prepare_firmware = fu_cros_ec_usb_device_prepare_firmware;
	klass_device->setup = fu_cros_ec_usb_device_setup;
	klass_device->to_string = fu_cros_ec_usb_device_to_string;
	klass_device->write_firmware = fu_cros_ec_usb_device_write_firmware;
	klass_usb_device->open = fu_cros_ec_usb_device_open;
	klass_usb_device->probe = fu_cros_ec_usb_device_probe;
	klass_usb_device->close = fu_cros_ec_usb_device_close;
}
