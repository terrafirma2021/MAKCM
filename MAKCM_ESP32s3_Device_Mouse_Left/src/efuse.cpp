#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "Arduino.h"

void burn_usb_phy_sel_efuse() {
    bool already_burned = esp_efuse_read_field_bit(ESP_EFUSE_USB_PHY_SEL);
    
    if (already_burned) {
        return;
    }

    esp_err_t err = esp_efuse_write_field_bit(ESP_EFUSE_USB_PHY_SEL);
    
    if (err == ESP_OK) {
        Serial0.println("USB_PHY_SEL efuse burned successfully.");
        Serial1.println("USB_PHY_SEL efuse burned successfully.");
        esp_restart();
    } else if (err == ESP_ERR_NOT_SUPPORTED) {
        Serial0.println("Burning this efuse is not supported.");
        Serial1.println("Burning this efuse is not supported.");
    } else {
        Serial0.println("Failed to burn USB_PHY_SEL efuse.");
        Serial1.println("Failed to burn USB_PHY_SEL efuse.");
    }
}
