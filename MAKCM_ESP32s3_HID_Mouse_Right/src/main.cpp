#include "EspUsbHost.h"
#include "OtaProtocol.h"
#include "efuse.h"

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#if USB_IS_DEBUG
    #warning "DEBUG MODE ENABLED: USB host will not work! For logging purposes only."
#endif

#ifndef FIRMWARE_VERSION
    #error "FIRMWARE_VERSION is not defined! Please set FIRMWARE_VERSION in the build flags."
#endif

const char* firmware = TOSTRING(FIRMWARE_VERSION);

EspUsbHost usbHost;

void setup()
{
  Serial0.begin(4000000);
  // UART1 carries MKOT, so a full DATA frame (kMaxWireSize = 4124 B) must fit
  // in the driver ring with room to spare.  4096 was 28 bytes short of one
  // frame: MANIFEST (220 B on the wire) always got through, DATA never did,
  // and a dropped byte fails CRC so the board answers nothing at all -- which
  // the host can only report as an unexplained timeout.
  Serial1.setRxBufferSize(2 * static_cast<int>(makcm::ota::kMaxWireSize));
  Serial1.begin(5000000, SERIAL_8N1, 2, 1); // Swap RX/TX from ESP A
  delay(1000);
  pinMode(9, OUTPUT);
  Serial0.println("RIGHT: MCU Started");
  // No banner on Serial1: a leading 'M' collides with the MKOT frame magic
  // and trips the left bridge decoder into a frame error on every boot.
  Serial1.printf("ESPLOG_BOOT reset=%u heap=%u\n",
                 static_cast<unsigned>(esp_reset_reason()),
                 static_cast<unsigned>(ESP.getFreeHeap()));
  burn_usb_phy_sel_efuse();
  usbHost.begin();
}

void loop()
{
  // Clean
}
