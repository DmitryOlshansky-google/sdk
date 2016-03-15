// Copyright (c) 2016, the Dartino project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE.md file.

#ifndef PLATFORMS_STM_DISCO_DARTINO_SRC_ETHERNET_H_
#define PLATFORMS_STM_DISCO_DARTINO_SRC_ETHERNET_H_

#include <cmsis_os.h>
#include <stm32f7xx_hal.h>

#include <cinttypes>

#include "FreeRTOSIPConfig.h"
#include "platforms/stm/disco_dartino/src/device_manager.h"
#include "src/shared/platform.h"


struct NetworkParameters {
  uint8_t ipAddress[4];
  uint8_t netMask[4];
  uint8_t gatewayAddress[4];
  uint8_t DNSServerAddress[4];
};

void GetNetworkAddressConfiguration(NetworkParameters * parameters);
BaseType_t InitializeNetworkStack(NetworkParameters const * parameters);
uint8_t IsNetworkUp();
uint32_t GetEthernetAdapterStatus();

#endif  // PLATFORMS_STM_DISCO_DARTINO_SRC_ETHERNET_H_
