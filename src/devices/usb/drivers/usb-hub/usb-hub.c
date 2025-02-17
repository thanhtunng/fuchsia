// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/hardware/usb/bus/c/banjo.h>
#include <fuchsia/hardware/usb/c/banjo.h>
#include <fuchsia/hardware/usb/hub/c/banjo.h>
#include <fuchsia/hardware/usb/hubdescriptor/c/banjo.h>
#include <inttypes.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>
#include <lib/sync/completion.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <unistd.h>
#include <zircon/listnode.h>

#include <usb/usb-request.h>
#include <usb/usb.h>

#include "src/devices/usb/drivers/usb-hub/usb_hub_bind.h"

// usb_port_status_t.wPortStatus
typedef uint16_t port_status_t;

typedef struct usb_hub {
  // the device we are publishing
  zx_device_t* zxdev;

  // Underlying USB device
  zx_device_t* usb_device;
  usb_protocol_t usb;

  usb_bus_protocol_t bus;

  usb_speed_t hub_speed;
  int num_ports;
  // delay after port power in microseconds
  zx_time_t power_on_delay;

  usb_request_t* status_request;
  sync_completion_t completion;

  thrd_t thread;
  atomic_bool thread_done;

  // port status values for our ports
  // length is num_ports
  port_status_t* port_status;

  size_t parent_req_size;

  // bit field indicating which ports have devices attached
  uint8_t attached_ports[128 / 8];
} usb_hub_t;

bool usb_hub_is_port_attached(usb_hub_t* hub, int port) {
  return (hub->attached_ports[port / 8] & (1 << (port % 8))) != 0;
}

void usb_hub_set_port_attached(usb_hub_t* hub, int port, bool enabled) {
  if (enabled) {
    hub->attached_ports[port / 8] |= (1 << (port % 8));
  } else {
    hub->attached_ports[port / 8] &= ~(1 << (port % 8));
  }
}

static zx_status_t usb_hub_get_port_status(usb_hub_t* hub, int port, port_status_t* out_status) {
  usb_port_status_t status;

  size_t out_length;
  zx_status_t result = usb_get_status(&hub->usb, USB_RECIP_PORT, port, &status, sizeof(status),
                                      ZX_TIME_INFINITE, &out_length);
  if (result != ZX_OK) {
    return result;
  }
  if (out_length != sizeof(status)) {
    return ZX_ERR_BAD_STATE;
  }

  zxlogf(DEBUG, "usb_hub_get_port_status port %d ", port);

  uint16_t port_change = status.w_port_change;
  if (port_change & USB_C_PORT_CONNECTION) {
    zxlogf(DEBUG, "USB_C_PORT_CONNECTION ");
    usb_clear_feature(&hub->usb, USB_RECIP_PORT, USB_FEATURE_C_PORT_CONNECTION, port,
                      ZX_TIME_INFINITE);
  }
  if (port_change & USB_C_PORT_ENABLE) {
    zxlogf(DEBUG, "USB_C_PORT_ENABLE ");
    usb_clear_feature(&hub->usb, USB_RECIP_PORT, USB_FEATURE_C_PORT_ENABLE, port, ZX_TIME_INFINITE);
  }
  if (port_change & USB_C_PORT_SUSPEND) {
    zxlogf(DEBUG, "USB_C_PORT_SUSPEND ");
    usb_clear_feature(&hub->usb, USB_RECIP_PORT, USB_FEATURE_C_PORT_SUSPEND, port,
                      ZX_TIME_INFINITE);
  }
  if (port_change & USB_C_PORT_OVER_CURRENT) {
    zxlogf(DEBUG, "USB_C_PORT_OVER_CURRENT ");
    usb_clear_feature(&hub->usb, USB_RECIP_PORT, USB_FEATURE_C_PORT_OVER_CURRENT, port,
                      ZX_TIME_INFINITE);
  }
  if (port_change & USB_C_PORT_RESET) {
    zxlogf(DEBUG, "USB_C_PORT_RESET");
    usb_clear_feature(&hub->usb, USB_RECIP_PORT, USB_FEATURE_C_PORT_RESET, port, ZX_TIME_INFINITE);
  }
  if (port_change & USB_C_BH_PORT_RESET) {
    zxlogf(DEBUG, "USB_C_BH_PORT_RESET");
    usb_clear_feature(&hub->usb, USB_RECIP_PORT, USB_FEATURE_C_BH_PORT_RESET, port,
                      ZX_TIME_INFINITE);
  }
  if (port_change & USB_C_PORT_LINK_STATE) {
    zxlogf(DEBUG, "USB_C_PORT_LINK_STATE");
    usb_clear_feature(&hub->usb, USB_RECIP_PORT, USB_FEATURE_C_PORT_LINK_STATE, port,
                      ZX_TIME_INFINITE);
  }
  if (port_change & USB_C_PORT_CONFIG_ERROR) {
    zxlogf(DEBUG, "USB_C_PORT_CONFIG_ERROR");
    usb_clear_feature(&hub->usb, USB_RECIP_PORT, USB_FEATURE_C_PORT_CONFIG_ERROR, port,
                      ZX_TIME_INFINITE);
  }
  zxlogf(DEBUG, "");

  *out_status = status.w_port_status;
  return ZX_OK;
}

static zx_status_t usb_hub_wait_for_port(usb_hub_t* hub, int port, port_status_t* out_status,
                                         port_status_t status_bits, port_status_t status_mask,
                                         zx_time_t stable_time) {
  const zx_time_t timeout = ZX_SEC(2);       // 2 second total timeout
  const zx_time_t poll_delay = ZX_MSEC(25);  // poll every 25 milliseconds
  zx_time_t total = 0;
  zx_time_t stable = 0;

  while (total < timeout) {
    zx_nanosleep(zx_deadline_after(poll_delay));
    total += poll_delay;

    zx_status_t result = usb_hub_get_port_status(hub, port, out_status);
    if (result != ZX_OK) {
      return result;
    }
    hub->port_status[port] = *out_status;

    if ((*out_status & status_mask) == status_bits) {
      stable += poll_delay;
      if (stable >= stable_time) {
        return ZX_OK;
      }
    } else {
      stable = 0;
    }
  }

  return ZX_ERR_TIMED_OUT;
}

static void usb_hub_interrupt_complete(void* ctx, usb_request_t* req) {
  zxlogf(DEBUG, "usb_hub_interrupt_complete got %d %" PRIu64 "", req->response.status,
         req->response.actual);
  usb_hub_t* hub = (usb_hub_t*)ctx;
  sync_completion_signal(&hub->completion);
}

static void usb_hub_power_on_port(usb_hub_t* hub, int port) {
  usb_set_feature(&hub->usb, USB_RECIP_PORT, USB_FEATURE_PORT_POWER, port, ZX_TIME_INFINITE);
  usleep(hub->power_on_delay);
}

static void usb_hub_port_enabled(usb_hub_t* hub, int port) {
  port_status_t status;

  zxlogf(DEBUG, "port %d usb_hub_port_enabled", port);

  // USB 2.0 spec section 9.1.2 recommends 100ms delay before enumerating
  // wait for USB_PORT_ENABLE == 1 and USB_PORT_RESET == 0
  if (usb_hub_wait_for_port(hub, port, &status, USB_PORT_ENABLE, USB_PORT_ENABLE | USB_PORT_RESET,
                            ZX_MSEC(100)) != ZX_OK) {
    zxlogf(ERROR, "usb_hub_wait_for_port USB_PORT_RESET failed for USB hub, port %d", port);
    return;
  }

  usb_speed_t speed;
  if (hub->hub_speed == USB_SPEED_SUPER) {
    speed = USB_SPEED_SUPER;
  } else if (status & USB_PORT_LOW_SPEED) {
    speed = USB_SPEED_LOW;
  } else if (status & USB_PORT_HIGH_SPEED) {
    speed = USB_SPEED_HIGH;
  } else {
    speed = USB_SPEED_FULL;
  }

  zxlogf(DEBUG, "call hub_device_added for port %d", port);
  usb_bus_device_added(&hub->bus, hub->usb_device, port, speed);
  usb_hub_set_port_attached(hub, port, true);
}

static void usb_hub_port_connected(usb_hub_t* hub, int port) {
  port_status_t status;

  zxlogf(DEBUG, "port %d usb_hub_port_connected", port);

  // USB 2.0 spec section 7.1.7.3 recommends 100ms between connect and reset
  if (usb_hub_wait_for_port(hub, port, &status, USB_PORT_CONNECTION, USB_PORT_CONNECTION,
                            ZX_MSEC(100)) != ZX_OK) {
    zxlogf(ERROR, "usb_hub_wait_for_port USB_PORT_CONNECTION failed for USB hub, port %d", port);
    return;
  }

  usb_set_feature(&hub->usb, USB_RECIP_PORT, USB_FEATURE_PORT_RESET, port, ZX_TIME_INFINITE);
  usb_hub_port_enabled(hub, port);
}

static zx_status_t usb_hub_port_reset(void* ctx, uint32_t port) {
  port_status_t status;
  usb_hub_t* hub = ctx;

  usb_set_feature(&hub->usb, USB_RECIP_PORT, USB_FEATURE_PORT_RESET, port, ZX_TIME_INFINITE);
  status = usb_hub_wait_for_port(hub, port, &status, USB_PORT_ENABLE,
                                 USB_PORT_ENABLE | USB_PORT_RESET, ZX_MSEC(100));
  if (status != ZX_OK) {
    zxlogf(ERROR, "usb_hub_wait_for_port USB_PORT_RESET failed for USB hub, port %d", port);
  }
  return status;
}

static usb_hub_interface_protocol_ops_t _hub_interface = {
    .reset_port = usb_hub_port_reset,
};

static void usb_hub_port_disconnected(usb_hub_t* hub, int port) {
  zxlogf(DEBUG, "port %d usb_hub_port_disconnected", port);
  usb_bus_device_removed(&hub->bus, hub->usb_device, port);
  usb_hub_set_port_attached(hub, port, false);
}

static void usb_hub_handle_port_status(usb_hub_t* hub, int port, port_status_t status) {
  port_status_t old_status = hub->port_status[port];

  zxlogf(DEBUG, "usb_hub_handle_port_status port: %d status: %04X old_status: %04X", port, status,
         old_status);

  hub->port_status[port] = status;

  if ((status & USB_PORT_CONNECTION) && !(status & USB_PORT_ENABLE)) {
    // Handle race condition where device is quickly disconnected and reconnected.
    // This happens when Android devices switch USB configurations.
    // In this case, any change to the connect state should trigger a disconnect
    // before handling a connect event.
    if (usb_hub_is_port_attached(hub, port)) {
      usb_hub_port_disconnected(hub, port);
      old_status &= ~USB_PORT_CONNECTION;
    }
  }
  bool port_enable_cleared = !(status & USB_PORT_ENABLE) && (old_status & USB_PORT_ENABLE);

  if ((status & USB_PORT_CONNECTION) && !(old_status & USB_PORT_CONNECTION)) {
    usb_hub_port_connected(hub, port);
    // Check for both USB_PORT_CONNECTION and USB_PORT_ENABLE below.
    // It seems to be a quirk of newer x86 hardware that USB_PORT_ENABLE might be set
    // but USB_PORT_CONNECTION is not.
  } else if (!(status & USB_PORT_CONNECTION) &&
             (old_status & USB_PORT_CONNECTION || port_enable_cleared)) {
    usb_hub_port_disconnected(hub, port);
  } else if ((status & USB_PORT_ENABLE) && !(old_status & USB_PORT_ENABLE)) {
    usb_hub_port_enabled(hub, port);
  }
}

static void usb_hub_unbind(void* ctx) {
  usb_hub_t* hub = ctx;
  for (int port = 1; port <= hub->num_ports; port++) {
    if (usb_hub_is_port_attached(hub, port)) {
      usb_hub_port_disconnected(hub, port);
    }
  }

  atomic_store(&hub->thread_done, true);
  sync_completion_signal(&hub->completion);
  if (hub->thread) {
    thrd_join(hub->thread, NULL);
  }

  device_unbind_reply(hub->zxdev);
}

static zx_status_t usb_hub_free(usb_hub_t* hub) {
  usb_request_release(hub->status_request);
  free(hub->port_status);
  free(hub);
  return ZX_OK;
}

static void usb_hub_release(void* ctx) {
  usb_hub_t* hub = ctx;
  usb_hub_free(hub);
}

static int usb_hub_thread(void* arg) {
  usb_hub_t* hub = (usb_hub_t*)arg;
  usb_request_t* req = hub->status_request;

  usb_hub_descriptor_t hub_desc;
  size_t out_length;
  int desc_type = (hub->hub_speed == USB_SPEED_SUPER ? USB_HUB_DESC_TYPE_SS : USB_HUB_DESC_TYPE);
  zx_status_t result =
      usb_get_descriptor(&hub->usb, USB_TYPE_CLASS | USB_RECIP_DEVICE, desc_type, 0,
                         (void*)&hub_desc, sizeof(hub_desc), ZX_TIME_INFINITE, &out_length);
  if (result < 0) {
    zxlogf(ERROR, "get hub descriptor failed: %d", result);
    device_init_reply(hub->zxdev, result, NULL);
    return result;
  } else if (hub_desc.b_desc_length != out_length) {
    zxlogf(ERROR, "usb_hub_descriptor_t.bDescLength != out_length");
    result = ZX_ERR_BAD_STATE;
    device_init_reply(hub->zxdev, result, NULL);
    return result;
  }

  // The length of the descriptor varies depending on whether it is USB 2.0 or 3.0,
  // and how many ports it has.
  size_t min_length = 7;
  size_t max_length = sizeof(hub_desc);
  if (out_length < min_length || out_length > max_length) {
    zxlogf(ERROR, "get hub descriptor got length %lu, want length between %lu and %lu", out_length,
           min_length, max_length);
    result = ZX_ERR_BAD_STATE;
    device_init_reply(hub->zxdev, result, NULL);
    return result;
  }

  // Determine whether the hub supports a single or multiple transaction-translators.  For
  // low or full-speed devices, this information is encoded in the b_device_protocol field of a
  // DEVICE_QUALIFIER descriptor.  For high-speed devices, this information is encoded in the
  // b_device_protocol field of a DEVICE descriptor.  See: USB 2.0 spec. 11.23.
  bool multi_tt = false;
  if (hub->hub_speed == USB_SPEED_LOW || hub->hub_speed == USB_SPEED_FULL) {
    usb_device_qualifier_descriptor_t qual_desc;
    result =
        usb_get_descriptor(&hub->usb, USB_TYPE_STANDARD, USB_DT_DEVICE_QUALIFIER, 0,
                           (void*)&qual_desc, sizeof(qual_desc), ZX_TIME_INFINITE, &out_length);
    if (result < 0) {
      zxlogf(ERROR, "get device_qualifier descriptor failed: %d", result);
      device_init_reply(hub->zxdev, result, NULL);
      return result;
    } else if (qual_desc.b_length != out_length) {
      zxlogf(ERROR, "usb_device_qualifier_descriptor_t.b_length != out_length");
      result = ZX_ERR_BAD_STATE;
      device_init_reply(hub->zxdev, result, NULL);
      return result;
    } else if (out_length != sizeof(qual_desc)) {
      zxlogf(ERROR, "get device_qualifier descriptor returned %ld bytes, want %ld", out_length,
             sizeof(qual_desc));
      result = ZX_ERR_BAD_STATE;
      device_init_reply(hub->zxdev, result, NULL);
      return result;
    }
    multi_tt = qual_desc.b_device_protocol == 2;
  } else if (hub->hub_speed == USB_SPEED_HIGH) {
    usb_device_descriptor_t dev_desc;
    result = usb_get_descriptor(&hub->usb, USB_TYPE_STANDARD, USB_DT_DEVICE, 0, (void*)&dev_desc,
                                sizeof(dev_desc), ZX_TIME_INFINITE, &out_length);
    if (result < 0) {
      zxlogf(ERROR, "get device descriptor failed: %d", result);
      device_init_reply(hub->zxdev, result, NULL);
      return result;
    } else if (dev_desc.b_length != out_length) {
      zxlogf(ERROR, "usb_device_descriptor_t.b_length != out_length");
      result = ZX_ERR_BAD_STATE;
      device_init_reply(hub->zxdev, result, NULL);
      return result;
    } else if (out_length != sizeof(dev_desc)) {
      zxlogf(ERROR, "get device descriptor returned %ld bytes, want %ld", out_length,
             sizeof(dev_desc));
      result = ZX_ERR_BAD_STATE;
      device_init_reply(hub->zxdev, result, NULL);
      return result;
    }
    multi_tt = dev_desc.b_device_protocol == 2;
  } else {  // super-speed
    // USB 3.x devices do not support the concept of transaction-translators.
    multi_tt = false;
  }

  result = usb_bus_configure_hub(&hub->bus, hub->usb_device, hub->hub_speed, &hub_desc, multi_tt);
  if (result < 0) {
    zxlogf(ERROR, "configure_hub failed: %d", result);
    device_init_reply(hub->zxdev, result, NULL);
    return result;
  }

  int num_ports = hub_desc.b_nbr_ports;

  if (num_ports > 127) {
    zxlogf(ERROR, "malformed descriptor: usb_hub_descriptor_t.bNbrPorts > 127");
    result = ZX_ERR_BAD_STATE;
    device_init_reply(hub->zxdev, result, NULL);
    return result;
  }

  hub->num_ports = num_ports;
  hub->port_status = calloc(num_ports + 1, sizeof(port_status_t));
  if (!hub->port_status) {
    result = ZX_ERR_NO_MEMORY;
    device_init_reply(hub->zxdev, result, NULL);
    return result;
  }

  // power on delay in microseconds
  hub->power_on_delay = hub_desc.b_power_on2_pwr_good * 2 * 1000;
  if (hub->power_on_delay < 100 * 1000) {
    // USB 2.0 spec section 9.1.2 recommends at least 100ms delay after power on
    hub->power_on_delay = 100 * 1000;
  }

  for (int i = 1; i <= num_ports; i++) {
    usb_hub_power_on_port(hub, i);
  }

  device_init_reply(hub->zxdev, ZX_OK, NULL);

  // bit field for port status bits
  uint8_t status_buf[128 / 8];
  memset(status_buf, 0, sizeof(status_buf));

  usb_request_complete_callback_t complete = {
      .callback = usb_hub_interrupt_complete,
      .ctx = hub,
  };

  // This loop handles events from our interrupt endpoint
  while (1) {
    sync_completion_reset(&hub->completion);
    usb_request_queue(&hub->usb, req, &complete);
    sync_completion_wait(&hub->completion, ZX_TIME_INFINITE);
    if (req->response.status != ZX_OK || atomic_load(&hub->thread_done)) {
      device_async_remove(hub->zxdev);
      return ZX_ERR_IO_INVALID;
    }

    __UNUSED size_t result = usb_request_copy_from(req, status_buf, req->response.actual, 0);
    uint8_t* bitmap = status_buf;
    uint8_t* bitmap_end = bitmap + req->response.actual;

    // bit zero is hub status
    if (bitmap[0] & 1) {
      // what to do here?
      zxlogf(ERROR, "usb_hub_interrupt_complete hub status changed");
    }

    int port = 1;
    int bit = 1;
    while (bitmap < bitmap_end && port <= num_ports) {
      if (*bitmap & (1 << bit)) {
        port_status_t status;
        zx_status_t result = usb_hub_get_port_status(hub, port, &status);
        if (result == ZX_OK) {
          usb_hub_handle_port_status(hub, port, status);
        }
      }
      port++;
      if (++bit == 8) {
        bitmap++;
        bit = 0;
      }
    }
  }

  return ZX_OK;
}

static void usb_hub_init(void* ctx) {
  usb_hub_t* hub = ctx;
  usb_bus_set_hub_interface(&hub->bus, hub->usb_device, hub, &_hub_interface);

  int ret = thrd_create_with_name(&hub->thread, usb_hub_thread, hub, "usb_hub_thread");
  if (ret != thrd_success) {
    device_init_reply(hub->zxdev, ZX_ERR_NO_MEMORY, NULL);
  }
}

static zx_protocol_device_t usb_hub_device_proto = {
    .version = DEVICE_OPS_VERSION,
    .init = usb_hub_init,
    .unbind = usb_hub_unbind,
    .release = usb_hub_release,
};

static zx_status_t usb_hub_bind(void* ctx, zx_device_t* device) {
  usb_protocol_t usb;
  zx_status_t status = device_get_protocol(device, ZX_PROTOCOL_USB, &usb);
  if (status != ZX_OK) {
    return status;
  }

  usb_bus_protocol_t bus;
  status = device_get_protocol(device, ZX_PROTOCOL_USB_BUS, &bus);
  if (status != ZX_OK) {
    zxlogf(ERROR, "usb_hub_bind could not find bus device");
    return status;
  }

  usb_desc_iter_t iter;
  status = usb_desc_iter_init(&usb, &iter);
  if (status < 0) {
    return status;
  }

  usb_interface_descriptor_t* intf = usb_desc_iter_next_interface(&iter, true);
  if (!intf || intf->b_num_endpoints != 1) {
    usb_desc_iter_release(&iter);
    return ZX_ERR_NOT_SUPPORTED;
  }

  usb_endpoint_descriptor_t* endp = usb_desc_iter_next_endpoint(&iter);
  if (!endp || usb_ep_type(endp) != USB_ENDPOINT_INTERRUPT) {
    usb_desc_iter_release(&iter);
    return ZX_ERR_NOT_SUPPORTED;
  }

  usb_ss_ep_comp_descriptor_t* ss_comp_desc = NULL;
  usb_descriptor_header_t* desc = usb_desc_iter_peek(&iter);
  if (desc && desc->b_descriptor_type == USB_DT_SS_EP_COMPANION) {
    ss_comp_desc = usb_desc_iter_get_structure(&iter, sizeof(usb_ss_ep_comp_descriptor_t));
  }
  usb_desc_iter_advance(&iter);

  uint8_t ep_addr = endp->b_endpoint_address;
  uint16_t max_packet_size = usb_ep_max_packet(endp);

  usb_hub_t* hub = calloc(1, sizeof(usb_hub_t));
  if (!hub) {
    zxlogf(ERROR, "Not enough memory for usb_hub_t.");
    usb_desc_iter_release(&iter);
    return ZX_ERR_NO_MEMORY;
  }
  atomic_init(&hub->thread_done, false);

  hub->usb_device = device;
  hub->hub_speed = usb_get_speed(&usb);
  memcpy(&hub->usb, &usb, sizeof(usb_protocol_t));
  memcpy(&hub->bus, &bus, sizeof(usb_bus_protocol_t));

  hub->parent_req_size = usb_get_request_size(&usb);

  usb_request_t* req;
  status = usb_request_alloc(&req, max_packet_size, ep_addr, hub->parent_req_size);
  if (status != ZX_OK) {
    usb_desc_iter_release(&iter);
    usb_hub_free(hub);
    return status;
  }
  hub->status_request = req;

  status = usb_enable_endpoint(&usb, endp, ss_comp_desc, true);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: usb_enable_endpoint failed %d", __FUNCTION__, status);
    usb_desc_iter_release(&iter);
    usb_hub_free(hub);
    return status;
  }

  usb_desc_iter_release(&iter);

  device_add_args_t args = {
      .version = DEVICE_ADD_ARGS_VERSION,
      .name = "usb-hub",
      .ctx = hub,
      .ops = &usb_hub_device_proto,
      .flags = DEVICE_ADD_NON_BINDABLE,
  };

  status = device_add(hub->usb_device, &args, &hub->zxdev);
  if (status != ZX_OK) {
    usb_hub_free(hub);
    return status;
  }

  return ZX_OK;
}

static zx_driver_ops_t usb_hub_driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = usb_hub_bind,
};

ZIRCON_DRIVER(usb_hub, usb_hub_driver_ops, "zircon", "0.1");
