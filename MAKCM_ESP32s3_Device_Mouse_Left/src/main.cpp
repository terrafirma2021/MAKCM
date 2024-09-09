#include "main.h"
#include "efuse.h"

void setup() {
    delay(1100);
    Serial0.begin(115200);
    pinMode(9, OUTPUT);
    digitalWrite(9, LOW);

    Serial1.begin(5000000, SERIAL_8N1, 1, 2);
    Serial1.println("READY");
    Serial0.println("Left MCU Started");

    Serial1.onReceive(serial1ISR);
    Serial0.onReceive(serial0ISR);
    burn_usb_phy_sel_efuse();

    tasks();
}

void loop() {
    requestUSBDescriptors();
}
