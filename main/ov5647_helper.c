#include "ov5647_helper.h"
#include "esp_log.h"

static const char *TAG = "OV5647_HELPER";
static i2c_master_bus_handle_t s_i2c_bus_handle = NULL;

void ov5647_helper_init(i2c_master_bus_handle_t bus_handle)
{
    s_i2c_bus_handle = bus_handle;
}

esp_err_t ov5647_read_reg(uint16_t reg, uint8_t *value)
{
    if (s_i2c_bus_handle == NULL) {
        ESP_LOGE(TAG, "I2C bus handle not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t reg_addr[2] = {
        (reg >> 8) & 0xFF,
        reg & 0xFF
    };
    
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x36,
        .scl_speed_hz = 100000,
    };
    
    i2c_master_dev_handle_t dev_handle;
    esp_err_t ret = i2c_master_bus_add_device(s_i2c_bus_handle, &dev_cfg, &dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device: %d", ret);
        return ret;
    }
    
    ret = i2c_master_transmit_receive(dev_handle, reg_addr, 2, value, 1, 100);
    i2c_master_bus_rm_device(dev_handle);
    
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Read OV5647 reg 0x%04X failed", reg);
    }
    return ret;
}

esp_err_t ov5647_write_reg(uint16_t reg, uint8_t value)
{
    if (s_i2c_bus_handle == NULL) {
        ESP_LOGE(TAG, "I2C bus handle not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t data[3] = {
        (reg >> 8) & 0xFF,  // MSB of register address
        reg & 0xFF,         // LSB of register address
        value               // value
    };
    
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x36,  // OV5647 I2C address
        .scl_speed_hz = 100000,
    };
    
    i2c_master_dev_handle_t dev_handle;
    esp_err_t ret = i2c_master_bus_add_device(s_i2c_bus_handle, &dev_cfg, &dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device: %d", ret);
        return ret;
    }
    
    ret = i2c_master_transmit(dev_handle, data, 3, 100);
    i2c_master_bus_rm_device(dev_handle);
    
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Write OV5647 reg 0x%04X = 0x%02X failed", reg, value);
    } else {
        ESP_LOGI(TAG, "OV5647 reg 0x%04X = 0x%02X OK", reg, value);
    }
    return ret;
}

void ov5647_set_exposure(uint32_t exposure_lines)
{
    // Exposure in OV5647 is a 20-bit value in registers 0x3500-0x3502
    // Value is number of lines * 16 (4 bits fraction)
    uint32_t exp_val = exposure_lines << 4;
    
    ov5647_write_reg(0x3500, (exp_val >> 16) & 0x0F);  // [19:16]
    ov5647_write_reg(0x3501, (exp_val >> 8) & 0xFF);   // [15:8]
    ov5647_write_reg(0x3502, exp_val & 0xF0);          // [7:4] (lower 4 bits always 0)
}

void ov5647_set_gain(uint16_t gain)
{
    ov5647_write_reg(0x350A, (gain >> 8) & 0x03);  // [9:8]
    ov5647_write_reg(0x350B, gain & 0xFF);         // [7:0]
}

void ov5647_set_awb(uint16_t red_gain, uint16_t green_gain, uint16_t blue_gain)
{
    // Enable manual AWB
    ov5647_write_reg(0x3406, 0x01);
    
    // Red gain (12-bit)
    ov5647_write_reg(0x3400, (red_gain >> 8) & 0x0F);
    ov5647_write_reg(0x3401, red_gain & 0xFF);
    
    // Green gain (12-bit)
    ov5647_write_reg(0x3402, (green_gain >> 8) & 0x0F);
    ov5647_write_reg(0x3403, green_gain & 0xFF);
    
    // Blue gain (12-bit)
    ov5647_write_reg(0x3404, (blue_gain >> 8) & 0x0F);
    ov5647_write_reg(0x3405, blue_gain & 0xFF);
    
    ESP_LOGI(TAG, "OV5647 AWB: R=0x%03X G=0x%03X B=0x%03X", red_gain, green_gain, blue_gain);
}

void ov5647_set_test_pattern(uint8_t pattern)
{
    // 0x503D: ISP CTRL 3D (Test pattern)
    ov5647_write_reg(0x503D, pattern);
    ESP_LOGI(TAG, "OV5647 Test Pattern: 0x%02X", pattern);
}

void ov5647_set_vts(uint16_t vts)
{
    ov5647_write_reg(0x380E, (vts >> 8) & 0xFF);
    ov5647_write_reg(0x380F, vts & 0xFF);
    ESP_LOGI(TAG, "OV5647 VTS set to: %d", vts);
}

uint16_t ov5647_get_vts(void)
{
    uint8_t hi = 0, lo = 0;
    ov5647_read_reg(0x380E, &hi);
    ov5647_read_reg(0x380F, &lo);
    uint16_t vts = (hi << 8) | lo;
    ESP_LOGI(TAG, "OV5647 VTS read: %d", vts);
    return vts;
}

void setup_camera_controls(void)
{
    ESP_LOGI(TAG, "Setting up OV5647 via I2C (official documentation)");

    // ===== ISP (Image Signal Processor) =====
    // 0x5000: lenc_en, bc_en, wc_en - all enabled
    ov5647_write_reg(0x5000, 0xFF);
    // 0x5001 bit[1]: awb_en - enable AWB in ISP
    ov5647_write_reg(0x5001, 0x03);  // AWB + win_en
    
    // ===== AEC/AGC =====
    // 0x3503: bit[0]=AEC manual, bit[1]=AGC manual
    // 0x00 = auto AE + auto AG
    ov5647_write_reg(0x3503, 0x00);
    
    // ===== AWB - AUTO (manual messes up colors) =====
    // 0x5180 bit[3]=0: auto gains (not manual)
    ov5647_write_reg(0x5180, 0x00);  // Auto AWB gains
    
    ESP_LOGI(TAG, "OV5647: ISP on, AEC/AGC/AWB all auto");
}
