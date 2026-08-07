#include "main.h"
#include "efuse.h"
#include "BridgeTransport.h"
#include "InputMonitor.h"

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
    // Secondary can burst km.move() at close to 1 kHz. The Arduino default
    // UART0 RX buffer is too small when USB and the 5 Mbps bridge are busy.
    Serial0.setRxBufferSize(4096);
    Serial0.begin(115200);
    pinMode(9, OUTPUT);
    digitalWrite(9, LOW);
    // A DSE Edge report descriptor is 405 bytes and is transported as several
    // back-to-back bridge frames. The Arduino default UART RX buffer is too
    // small for that control-plane burst at 5 Mbps.
    Serial1.setRxBufferSize(4096);
    Serial1.begin(5000000, SERIAL_8N1, 1, 2);
    
    if (USB_IS_DEBUG) {
        Serial0.println("WARNING: Debug mode is enabled!");
        Serial0.println("USB Host will not work!");
        Serial0.println("For logging purposes only!!!\n");
    }

    Serial0.print("MAKCM version ");
    Serial0.print(firmware);  
    Serial0.println("\n");

    burn_usb_phy_sel_efuse();
    beginBridgeTransport();

    // Stay detached until the right MCU supplies the real device profile.
    Passthrough.resetProfile();
    beginInputMonitor();
    tasks();
    // Install callbacks only after their target tasks exist. Otherwise an
    // already-running right MCU can notify a null task handle during boot.
    Serial1.onReceive(serial1ISR);
}

void loop() {
    if (!USB_IS_DEBUG) {
        serviceBridgeTransport();
        requestUSBDescriptors();
    }
}
