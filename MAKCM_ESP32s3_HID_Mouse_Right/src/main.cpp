#include "EspUsbHost.h"
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
  Serial1.begin(5000000, SERIAL_8N1, 2, 1); // Swap RX/TX from ESP A
  delay(1000);
  pinMode(9, OUTPUT);
  usbHost.begin();
  Serial0.println("RIGHT: MCU Started");
  Serial1.println("MAKCK v1.2");
  burn_usb_phy_sel_efuse();
}

void loop()
{
  // Clean
}
