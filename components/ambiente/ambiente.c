#include <math.h>
#include "sdkconfig.h"
#include "dht.h"
#include "ambiente.h"
#include "sensor_sync.h"
#include "driver/gpio.h"

esp_err_t ambiente_ler(ambiente_leitura_t *leitura)
{
    if (!leitura) {
        return ESP_ERR_INVALID_ARG;
    }

    leitura->temperatura_c = NAN;
    leitura->umidade_pct = NAN;
    if (!GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_IKNET_DHT11_GPIO)) {
        return ESP_ERR_INVALID_ARG;
    }

    float temperatura, umidade;
    sensor_sync_lock();
    esp_err_t err = dht_read_float_data(DHT_TYPE_DHT11,
                                       CONFIG_IKNET_DHT11_GPIO,
                                       &umidade, &temperatura);
    sensor_sync_unlock();
    if (err != ESP_OK) {
        return err;
    }
    if (!isfinite(temperatura) || !isfinite(umidade) ||
        umidade < 0.0f || umidade > 100.0f) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    leitura->temperatura_c = temperatura;
    leitura->umidade_pct = umidade;
    return ESP_OK;
}