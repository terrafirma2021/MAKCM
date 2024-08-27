#include "EspUsbHost.h"

EspUsbHost usbHost;


void setup()
{
  Serial0.begin(500000);
  Serial1.begin(5000000, SERIAL_8N1, 2, 1); // Swap RX/TX from ESP A  SERIAL_8N1, 2, 1
  delay(1000);
  pinMode(9, OUTPUT);
  usbHost.begin();
  Serial1.println("MCU Started");
}

void loop()
{
// Clean 
}
