#include "EspUsbHost.h"
#include "efuse.h"

EspUsbHost usbHost;

void setup()
{
  Serial0.begin(115200);
  Serial1.begin(5000000, SERIAL_8N1, 2, 1); // Swap RX/TX from ESP A
  delay(1000);
  pinMode(9, OUTPUT);
  usbHost.begin();
  Serial0.println("RIGHT: MCU Started");
  Serial1.println("MCU Started");
  burn_usb_phy_sel_efuse();
}

void loop()
{
  // Clean
}
