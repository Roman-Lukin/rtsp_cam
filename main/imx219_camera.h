/**
 * @file imx219_camera.h
 * @brief IMX219 sensor implementation of camera interface
 * 
 * Sony IMX219 8MP sensor (Raspberry Pi Camera Module v2)
 * - I2C address: 0x10
 * - Bayer pattern: RGGB
 * - Max resolution: 3280x2464
 */

#pragma once

#include "camera_interface.h"

#ifdef __cplusplus

class IMX219Camera : public ICameraInterface {
public:
    explicit IMX219Camera(i2c_master_bus_handle_t i2c_bus);
    ~IMX219Camera() override = default;
    
    // ICameraInterface implementation
    const CameraCapabilities& getCapabilities() const override;
    esp_err_t verifyChipId() override;
    esp_err_t softReset() override;
    esp_err_t setStreaming(bool enable) override;
    
    void setExposure(uint16_t lines) override;
    uint16_t getExposure() override;
    esp_err_t setAutoExposure(bool enable) override;
    
    void setAnalogGain(uint16_t gain) override;
    uint16_t getAnalogGain() override;
    void setDigitalGain(uint16_t red, uint16_t green, uint16_t blue) override;
    
    void setTestPattern(TestPattern pattern) override;
    void setTestPatternRaw(uint16_t value) override;
    
    void setOrientation(bool hMirror, bool vFlip) override;
    void setFrameLength(uint16_t lines) override;
    uint16_t getFrameLength() override;
    
    void debugStatus() override;
    
private:
    // I2C helpers
    esp_err_t readReg(uint16_t reg, uint8_t* value);
    esp_err_t writeReg(uint16_t reg, uint8_t value);
    esp_err_t readReg16(uint16_t reg, uint16_t* value);
    esp_err_t writeReg16(uint16_t reg, uint16_t value);
    
    i2c_master_dev_handle_t i2c_dev_ = nullptr;
    
    static const CameraCapabilities capabilities_;
};

#endif // __cplusplus
