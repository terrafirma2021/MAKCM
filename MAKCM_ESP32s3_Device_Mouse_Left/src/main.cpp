#include "main.h"
#include "efuse.h"

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#if USB_IS_DEBUG
    #warning "DEBUG MODE ENABLED: USB host will not work! For logging purposes only."
#endif

// Ensure the FIRMWARE_VERSION is defined
#ifndef FIRMWARE_VERSION
    #error "FIRMWARE_VERSION is not defined! Please set FIRMWARE_VERSION in the build flags."
#endif

const char* firmware = TOSTRING(FIRMWARE_VERSION);

void setup() {
    delay(1100);
    Serial0.begin(115200);
    pinMode(9, OUTPUT);
    digitalWrite(9, LOW);
    Serial1.begin(5000000, SERIAL_8N1, 1, 2);
    
    if (USB_IS_DEBUG) {
        Serial0.println("WARNING: Debug mode is enabled!");
        Serial0.println("USB Host will not work!");
        Serial0.println("For logging purposes only!!!\n");
    }

    Serial0.print("MAKCM version ");
    Serial0.print(firmware);  
    Serial0.println("\n");

    Serial1.onReceive(serial1ISR);
    Serial0.onReceive(serial0ISR);
    burn_usb_phy_sel_efuse();
    tasks();
}

void loop() {
    if (!USB_IS_DEBUG) {
        requestUSBDescriptors();
    }
}
