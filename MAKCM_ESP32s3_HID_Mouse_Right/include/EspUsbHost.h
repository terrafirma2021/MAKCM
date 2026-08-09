#ifndef ESP_USB_HOST_H
#define ESP_USB_HOST_H

#include <Arduino.h>
#include <usb/usb_host.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include "freertos/queue.h"
#include <cstring>
#include <atomic>
#include <ArduinoJson.h>
#include <string>
#include "BridgeProtocol.h"
#include "DualSenseControllerAdapter.h"
#include "OtaReceiver.h"
#include "UsbHidTypes.h"

#define LOG_LEVEL_OFF    0
#define LOG_LEVEL_FIXED  1
#define LOG_LEVEL_INFO   2
#define LOG_LEVEL_WARN   3
#define LOG_LEVEL_ERROR  4
#define LOG_LEVEL_DEBUG  5
#define LOG_LEVEL_PARSED 6

#define LOG_QUEUE_SIZE 20
#define LOG_MESSAGE_SIZE 620

#define USB_ACTION_OPEN_DEVICE   0x01
#define USB_ACTION_CLOSE_DEVICE  0x02


void flashLEDToggleTask(void *parameter);
extern SemaphoreHandle_t ledSemaphore;

class EspUsbHost
{
public:
    static constexpr uint16_t DSE_EDGE_VID = 0x054C;
    static constexpr uint16_t DSE_EDGE_PID = 0x0DF2;
    static constexpr uint16_t COMPX_8K_VID = 0x373B;
    static constexpr uint16_t COMPX_8K_PID = 0x1278;

    enum class HostState : uint8_t {
        Empty = 0,
        Opening = 1,
        Active = 2,
        Cleaning = 3,
        OtaReserved = 4,
        Fault = 5,
    };

    struct DseTriggerState {
        volatile uint8_t l2 = 0;
        volatile uint8_t r2 = 0;
        volatile uint32_t reportCount = 0;
        volatile uint32_t lastReportMillis = 0;
    } dseTriggerState;

    struct DseSinkState {
        uint8_t lastOutput[makcm::bridge::kMaxPayloadSize] = {};
        uint8_t lastFeatureSet[makcm::bridge::kMaxPayloadSize] = {};
        volatile uint16_t lastOutputLength = 0;
        volatile uint16_t lastFeatureSetLength = 0;
        volatile uint16_t lastOutputSequence = 0;
        volatile uint16_t lastFeatureSetSequence = 0;
        volatile uint32_t outputCount = 0;
        volatile uint32_t featureSetCount = 0;
        volatile uint32_t bridgeRxErrors = 0;
        volatile uint32_t inputQueueOverwrites = 0;
        volatile uint32_t inputTxErrors = 0;
    } dseSinkState;

    // Debug and Log
    bool debugModeActive = false;
    bool isReady = false;
    static bool deviceMouseReady;;
    volatile bool streamStartPending = false;
    // Close the UART report gate while the Windows-facing USB device is
    // suspended.  Resume requires a fresh StreamStarted/StreamReady handshake.
    volatile bool downstreamSuspended = false;
    volatile uint32_t lastStreamStartedMillis = 0;
    volatile uint32_t streamStartRetries = 0;
    uint8_t interval;
    bool isClientRegistering = false;
    esp_err_t hostInstallStatus = ESP_FAIL;
    esp_err_t clientRegisterStatus = ESP_FAIL;
    volatile uint32_t newDeviceEventCount = 0;
    volatile uint32_t inputReportCount = 0;
    volatile uint32_t inputForwardCount = 0;
    volatile uint32_t activeSessionId = 0;
    volatile uint32_t closingSessionId = 0;
    volatile bool deviceGoneAccepted = false;
    uint32_t nextSessionId = 0;
    std::atomic<HostState> hostState{HostState::Empty};
    volatile uint32_t cleanupCount = 0;
    volatile uint32_t cleanupErrorCount = 0;
    volatile uint8_t pendingDeviceAddress = 0;
    volatile bool cleanupRequested = false;
    // Atomically reserves the USB Host lifecycle before OTA maintenance.
    // NEW_DEV callbacks queue the address while this latch is set instead of
    // racing Empty -> Opening against the OTA bootstrap task.
    std::atomic<bool> otaHostReserved{false};
    // OTA bootstrap is asynchronous.  The UART RX task must keep running
    // while the USB client task tears the physical device down; otherwise it
    // cannot consume DeviceGoneAccepted and cleanup waits on itself.
    static constexpr uint32_t kOtaBootstrapWaitMs = 3500;
    static constexpr uint32_t kEnumerationWaitMs = 5000;
    volatile bool otaBootstrapPending = false;
    volatile uint32_t otaBootstrapTransaction = 0;
    volatile uint32_t otaBootstrapPeerGeneration = 0;
    volatile uint32_t otaBootstrapStartedMs = 0;
    volatile uint32_t profileStableSinceMs = 0;
    static bool deviceConnected;
    uint32_t last_activity_time;
    TaskHandle_t cleanupTaskHandle = nullptr;

    uint8_t actionsPending = 0;
    uint8_t deviceAddress = 0;
    SemaphoreHandle_t mutex;
    SemaphoreHandle_t serial1Mutex = nullptr;
    SemaphoreHandle_t cleanupComplete = nullptr;
    static constexpr uint8_t kInputQueueCount = 16;
    static constexpr UBaseType_t kInputQueueDepth = 16;
    QueueHandle_t dseInputQueue = nullptr;
    QueueHandle_t endpointInputQueues[kInputQueueCount] = {};
    uint8_t inputQueueRoundRobin = 0;
    TaskHandle_t dseUartTxTaskHandle = nullptr;
    TaskHandle_t usbClientTaskHandle = nullptr;

    struct InputQueueItem {
        makcm::bridge::MessageType type = makcm::bridge::MessageType::EndpointIn;
        uint32_t session = 0;
        uint16_t length = 0;
        // Generic endpoint data carries endpoint + flags before up to 64
        // bytes of Full-Speed payload.
        uint8_t data[66] = {};
    };

    makcm::bridge::DeviceProfile passthroughProfile = {};
    uint8_t reportDescriptor[makcm::bridge::kMaxReportDescriptorSize] = {};
    volatile uint16_t reportDescriptorLength = 0;
    volatile bool reportDescriptorReady = false;
    uint8_t rawDeviceDescriptor[18] = {};
    uint8_t rawConfigurationDescriptor[
        makcm::bridge::kMaxConfigurationDescriptorSize] = {};
    uint16_t rawDeviceDescriptorLength = 0;
    uint16_t rawConfigurationDescriptorLength = 0;
    // Keep the raw configuration header fields separately from the copied
    // buffer.  These make a partial/incorrect host-cache capture visible in
    // ESPLOG_STATUS instead of silently advertising a truncated profile.
    uint16_t rawConfigurationDeclaredLength = 0;
    uint8_t rawConfigurationInterfaceCount = 0;
    uint16_t rawConfigurationCrc16 = 0;
    uint8_t rawBosDescriptor[makcm::bridge::kMaxBosDescriptorSize] = {};
    uint16_t rawBosDescriptorLength = 0;
    uint8_t rawQualifierDescriptor[
        makcm::bridge::kMaxQualifierDescriptorSize] = {};
    uint16_t rawQualifierDescriptorLength = 0;
    uint8_t optionalDescriptorStage = 0;
    bool rawTopologySupported = false;
    // The right host reports the same capability reason enum as the left
    // planner.  Keep this separate from the boolean because descriptor
    // capture can fail after a valid topology was already parsed (for
    // example, a HID report transfer can STALL).
    makcm::bridge::CapabilityReason rawTopologyRejectReason =
        makcm::bridge::CapabilityReason::None;

    struct ProxyHidDescriptor {
        uint8_t interfaceNumber = 0xFF;
        uint16_t expectedLength = 0;
        uint16_t receivedLength = 0;
        uint8_t data[makcm::bridge::kMaxReportDescriptorSize] = {};
    } proxyHidDescriptors[makcm::bridge::kMaxHidInterfaces];
    uint8_t proxyHidDescriptorCount = 0;
    uint8_t nextProxyHidDescriptor = 0;

    struct ProxyStringDescriptor {
        uint8_t index = 0;
        uint16_t langid = 0;
        uint16_t receivedLength = 0;
        uint8_t data[makcm::bridge::kMaxStringDescriptorSize] = {};
    } proxyStringDescriptors[makcm::bridge::kMaxStringDescriptors];
    uint8_t proxyStringDescriptorCount = 0;
    uint8_t nextProxyStringDescriptor = 0;
    uint16_t proxyStringLangid = 0x0409;
    uint16_t proxyLangIds[8] = {};
    uint8_t proxyLangIdCount = 0;
    uint8_t requiredStringIndices[32] = {};
    uint8_t requiredStringIndexCount = 0;

    struct ProxyControlCache {
        uint8_t bmRequestType = 0;
        uint8_t bRequest = 0;
        uint16_t wValue = 0;
        uint16_t wIndex = 0;
        uint16_t wLength = 0;
        uint16_t receivedLength = 0;
        uint8_t data[makcm::bridge::kMaxCachedControlSize] = {};
    } proxyControlCache[makcm::bridge::kMaxCachedControls];
    uint8_t proxyControlCacheCount = 0;
    uint8_t microsoftOsVendorCode = 0;

    struct ProxyEndpoint {
        uint8_t address = 0;
        uint8_t physicalAddress = 0;
        uint8_t interfaceNumber = 0xFF;
        uint8_t physicalInterfaceNumber = 0xFF;
        uint8_t attributes = 0;
        uint16_t packetSize = 0;
        uint8_t interval = 0;
    } proxyEndpoints[16];
    uint8_t proxyEndpointCount = 0;
    bool dualSenseControllerAdapter = false;
    makcm::bridge::DualSenseTransformResult dualSenseTransform{};
    uint8_t physicalConfigurationDescriptor[
        makcm::bridge::kMaxConfigurationDescriptorSize] = {};
    uint16_t physicalConfigurationDescriptorLength = 0;
    uint8_t selectedInterface = 0xFF;
    uint8_t selectedInEndpoint = 0;
    uint8_t selectedOutEndpoint = 0;
    uint16_t selectedInPacketSize = 0;
    uint16_t selectedOutPacketSize = 0;
    uint8_t auxiliaryMouseInterface = 0xFF;
    uint8_t auxiliaryMouseInEndpoint = 0;
    uint8_t auxiliaryMouseOutEndpoint = 0;
    uint16_t auxiliaryMouseInPacketSize = 0;
    uint16_t auxiliaryMouseOutPacketSize = 0;

    bool isDseEdgeDevice = false;
    bool isCompx8kMouseDevice = false;
    bool currentInterfaceClaimed = false;
    bool currentInterfaceIsAuxMouse = false;
    uint8_t currentInterfaceNumber = 0xFF;
    uint8_t currentPhysicalInterfaceNumber = 0xFF;
    uint8_t compxLastButtons = 0;

    enum Strutstate {
        STATE_IDLE,
        STATE_PROCESSING_COMMAND,
        STATE_SENDING_RESPONSE,
    };

    struct usb_string_descriptor_t {
        uint8_t bLength;
        uint8_t bDescriptorType;
        String data;
    };

    struct endpoint_data_t
    {
        uint8_t bInterfaceNumber;
        uint8_t bInterfaceClass;
        uint8_t bInterfaceSubClass;
        uint8_t bInterfaceProtocol;
        uint8_t bCountryCode;
    } endpoint_data_list[17];

    esp_err_t claim_err;
    usb_host_client_handle_t clientHandle;
    usb_device_handle_t deviceHandle;
    usb_transfer_t *usbTransfer[16];
    uint8_t usbTransferSize;
    uint8_t usbInterface[16];
    uint8_t usbInterfaceSize;

    TaskHandle_t usbTaskHandle = nullptr;
    TaskHandle_t clientTaskHandle = nullptr;

    struct HIDReportDescriptor {
        uint8_t reportId;
        uint8_t buttonSize;
        uint8_t xAxisSize;
        uint8_t yAxisSize;
        uint8_t wheelSize;
        uint8_t buttonStartByte;
        uint8_t xAxisStartByte;
        uint8_t yAxisStartByte;
        uint8_t wheelStartByte;
    };

    static struct HIDReportDescriptor HIDReportDesc;

    struct DeviceInfo {
        uint8_t speed;                         // USB device speed
        uint8_t dev_addr;                      // Device address
        uint8_t vMaxPacketSize0;               // Maximum packet size for endpoint 0
        uint8_t bConfigurationValue;           // Configuration value
        char str_desc_manufacturer[64];        // Manufacturer string descriptor
        char str_desc_product[64];             // Product string descriptor
        char str_desc_serial_num[64];          // Serial number string descriptor
    } device_info;

    struct DescriptorDevice {
        uint8_t bLength;                       // Descriptor length
        uint8_t bDescriptorType;               // Descriptor type
        uint16_t bcdUSB;                       // USB specification number
        uint8_t bDeviceClass;                  // Device class
        uint8_t bDeviceSubClass;               // Device subclass
        uint8_t bDeviceProtocol;               // Device protocol
        uint8_t bMaxPacketSize0;               // Maximum packet size for endpoint 0
        uint16_t idVendor;                     // Vendor ID
        uint16_t idProduct;                    // Product ID
        uint16_t bcdDevice;                    // Device release number
        uint8_t iManufacturer;                 // Index of manufacturer string descriptor
        uint8_t iProduct;                      // Index of product string descriptor
        uint8_t iSerialNumber;                 // Index of serial number string descriptor
        uint8_t bNumConfigurations;            // Number of possible configurations
    } descriptor_device;

    #define MAX_ENDPOINT_DESCRIPTORS 10
    struct usb_endpoint_descriptor_t {
        uint8_t bLength;           
        uint8_t bDescriptorType;   
        uint8_t bEndpointAddress;  
        uint8_t endpointID;        
        String direction;          
        uint8_t bmAttributes;      
        String attributes;        
        uint16_t wMaxPacketSize;   
        uint8_t bInterval;         
    } endpoint_descriptors[MAX_ENDPOINT_DESCRIPTORS];
    int endpointCounter = 0; 

    struct DescriptorConfiguration {
        uint8_t bLength;             // Descriptor length
        uint8_t bDescriptorType;     // Descriptor type
        uint16_t wTotalLength;       // Total length of data for this configuration
        uint8_t bNumInterfaces;      // Number of interfaces
        uint8_t bConfigurationValue; // Configuration value
        uint8_t iConfiguration;      // Index of string descriptor describing this configuration
        uint8_t bmAttributes;        // Configuration characteristics
        uint8_t bMaxPower;           // Maximum power consumption (in 2mA units)
    } descriptor_configuration;

    #define MAX_INTERFACE_DESCRIPTORS 10
    struct usb_interface_descriptor_t {
        uint8_t bLength;           
        uint8_t bDescriptorType;   
        uint8_t bInterfaceNumber;  
        uint8_t bAlternateSetting; 
        uint8_t bNumEndpoints;     
        uint8_t bInterfaceClass;   
        uint8_t bInterfaceSubClass;
        uint8_t bInterfaceProtocol;
        uint8_t iInterface;
    } interface_descriptors[MAX_INTERFACE_DESCRIPTORS];
    int interfaceCounter = 0;

    #define MAX_HID_DESCRIPTORS 10 
    struct usb_hid_descriptor_t {
        uint8_t bLength;        
        uint8_t bDescriptorType;
        uint16_t bcdHID;        
        uint8_t bCountryCode;   
        uint8_t bNumDescriptors;
        uint8_t bReportType;
        uint16_t wReportLength;
    } hid_descriptors[MAX_HID_DESCRIPTORS];
    int hidDescriptorCounter = 0;

    struct usb_iad_desc_t {
        uint8_t bLength;
        uint8_t bDescriptorType;
        uint8_t bFirstInterface;
        uint8_t bInterfaceCount;
        uint8_t bFunctionClass;
        uint8_t bFunctionSubClass;
        uint8_t bFunctionProtocol;
        uint8_t iFunction;
    } descriptor_interface_association;

 struct UsbConfigurationDescriptor {
        uint8_t bLength;
        uint8_t bDescriptorType;
        uint16_t wTotalLength;
        uint8_t bNumInterfaces;
        uint8_t bConfigurationValue;
        uint8_t iConfiguration;
        uint8_t bmAttributes;
        uint8_t bMaxPower;
    } configurationDescriptor;

    #define MAX_UNKNOWN_DESCRIPTORS 10

    struct usb_unknown_descriptor_t {
        uint8_t bLength;
        uint8_t bDescriptorType;
        String data;
    } unknown_descriptors[MAX_UNKNOWN_DESCRIPTORS];
    int unknownDescriptorCounter = 0;

    struct ParsedValues
    {
        uint8_t usagePage;
        uint8_t usage;
        uint8_t reportId;
        uint8_t reportSize;
        uint8_t reportCount;
        int16_t logicalMin16;
        int16_t logicalMax;
        int8_t logicalMin8;
        int8_t logicalMax8;
        uint8_t level;
        uint8_t size;
        uint8_t collection;
        uint16_t usageMinimum;
        uint16_t usageMaximum;
        bool hasReportId;
        uint16_t currentBitOffset;
    };

    void begin(void);
    static void receiveSerial1(void *parameter);
    static void dseUartTxTask(void *parameter);
    static void _clientEventCallback(const usb_host_client_event_msg_t *eventMsg, void *arg);
    static void _onReceiveControl(usb_transfer_t *transfer);
    static void monitorInactivity(void *arg);
    static void _onReceive(usb_transfer_t *transfer);
    void get_device_status();
    bool serial1Send(const char *format, ...);
    bool serial1Write(const uint8_t *data, size_t length);
    void sendStreamStarted();
    bool sendBridgeFrame(makcm::bridge::MessageType type,
                         uint16_t sequence,
                         const uint8_t *payload,
                         uint16_t length,
                         uint32_t session = UINT32_MAX);
    void handleBridgeFrame(const makcm::bridge::Frame &frame);
    void queueInputReport(makcm::bridge::MessageType type,
                          const uint8_t *data, uint16_t length);
    void resetInputQueues();
    void sendPassthroughProfile();
    void sendRuntimeStatus();
    bool submitOutputReport(const uint8_t *data, uint16_t length);
    bool submitEndpointOutput(uint8_t endpoint, const uint8_t *data,
                              uint16_t length);
    bool submitProxyControl(const makcm::bridge::ControlRequestHeader &request,
                            const uint8_t *data, uint16_t length,
                            uint32_t session);
    static void _onProxyControl(usb_transfer_t *transfer);
    void requestNextHidDescriptor();
    void prepareStringDescriptors();
    void buildStringDescriptorRequests();
    void requestNextStringDescriptor();
    bool submitStringDescriptor(uint8_t index, uint16_t langid);
    static void _onStringDescriptor(usb_transfer_t *transfer);
    void requestMicrosoftOsDescriptors();
    bool submitMicrosoftOsDescriptor(uint8_t slotIndex);
    static void _onMicrosoftOsDescriptor(usb_transfer_t *transfer);
    void requestOptionalDescriptors();
    bool submitOptionalDescriptor(makcm::bridge::ProfileBlobType type);
    static void _onOptionalDescriptor(usb_transfer_t *transfer);
    void sendControlCache();
    bool submitFeatureSet(const uint8_t *data, uint16_t length);
    bool submitFeatureGet(uint8_t reportId, uint16_t length);
    static void _onFeatureGet(usb_transfer_t *transfer);
    void onConfig(const uint8_t bDescriptorType, const uint8_t *p);
    static String getUsbDescString(const usb_str_desc_t *str_desc);
    esp_err_t submitControl(const uint8_t bmRequestType, const uint8_t bDescriptorIndex, const uint8_t bDescriptorType, const uint16_t wInterfaceNumber, const uint16_t wDescriptorLength);
    void _configCallback(const usb_config_desc_t *config_desc);
   HIDReportDescriptor parseHIDReportDescriptor(uint8_t *data, int length);
    virtual void onReceive(const usb_transfer_t *transfer) {};
    virtual void onGone(const usb_host_client_event_msg_t *eventMsg) {};
    virtual void onMouse(makcm::usb::BootMouseReport report,
                         uint8_t last_buttons);
    virtual void onMouseButtons(makcm::usb::BootMouseReport report,
                                uint8_t last_buttons);
    virtual void onMouseMove(makcm::usb::BootMouseReport report);
    void receiveSerial0(void *command);
    void logRawBytes(const char *functionName, const uint8_t *data, uint16_t length);
    // Device/interface teardown must run in the same client task context as
    // usb_host_client_handle_events().
    void cleanupDevice();
    void serviceOtaBootstrap();
    // Emits "OTA_UNAVAILABLE <site> <hostState>".  Eight distinct refusals
    // shared one bare line, which is why "busy with a device" and "can never
    // accept a bootstrap again" were indistinguishable from the host.
    void sendBootstrapRefusal(uint8_t site);

    uint8_t proxyToPhysicalEndpoint(uint8_t endpoint) const;
    uint8_t physicalToProxyEndpoint(uint8_t endpoint) const;
    uint8_t proxyToPhysicalInterface(uint8_t interfaceNumber) const;

    void sendDeviceInfo();
    void sendDescriptorDevice();
    void sendEndpointDescriptors();
    void sendInterfaceDescriptors();
    void sendHidDescriptors();
    void sendIADescriptors();
    void sendEndpointData();
    void sendUnknownDescriptors();
    void sendDescriptorconfig();
    void handleIncomingCommands(const String &command,
                                bool trustedMaintenanceSource);
    void usbLibraryTask(void *arg);
    void usbClientTask(void *arg);

    // USB fix


};


#endif
