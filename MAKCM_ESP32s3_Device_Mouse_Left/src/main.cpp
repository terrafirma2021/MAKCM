#include "main.h"
#include "efuse.h"

const char* firmware = "1.1";

void setup() {
    delay(1100);
    Serial0.begin(115200);
    pinMode(9, OUTPUT);
    digitalWrite(9, LOW);
    Serial1.begin(5000000, SERIAL_8N1, 1, 2);
    Serial0.print("MAKCM version ");
    Serial0.print(firmware);  
    Serial0.println("\n");

    Serial1.onReceive(serial1ISR);
    Serial0.onReceive(serial0ISR);
    burn_usb_phy_sel_efuse();
    tasks();
}

void loop() {
    requestUSBDescriptors();
}
