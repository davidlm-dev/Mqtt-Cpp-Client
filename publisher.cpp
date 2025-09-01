#include <algorithm>
#include <string>
#include <iostream>
#include <cstring>
#include <mosquitto.h>
#include <thread>
#include <random>
#include <chrono>
#include <vector>
#include <cmath>
#include <mutex>

// ===================== UTILS =====================
inline float clamp(float val, float min_val, float max_val) {
    return std::max(min_val, std::min(val, max_val));
}

// ===================== ESTADO GLOBAL =====================
struct EstadoGlobal {
    float presion;
    float nubosidad;   // 0-100%
    float radiacion;   // W/m²
    bool tormenta;
    bool ola_calor;
    std::string estacion_año;
};

// Motor global con mutex
struct MotorGlobal {
    float presion;
    float nubosidad;
    float radiacion;
    bool tormenta;
    bool ola_calor;
    std::string estacion_año;

    std::mutex mtx;

    EstadoGlobal snapshot() {
        std::lock_guard<std::mutex> lock(mtx);
        return {presion, nubosidad, radiacion, tormenta, ola_calor, estacion_año};
    }
};

// ===================== ESTACIONES =====================
struct Estacion {
    int id;
    std::string nombre;
    std::string region;
};

MotorGlobal motor;
std::random_device rd;
std::mt19937 gen(rd());

// ===================== MOTOR GLOBAL =====================
void motor_global_thread() {
    std::normal_distribution<> pres_delta(0.0, 0.2);
    std::normal_distribution<> nub_delta(0.0, 5.0);
    std::normal_distribution<> solar_delta(0.0, 10.0);
    std::uniform_real_distribution<> evento(0.0, 1.0);

    while (true) {
        auto now = std::chrono::system_clock::now();
        std::time_t now_c = std::chrono::system_clock::to_time_t(now);
        struct tm *lt = std::localtime(&now_c);
        int hora = lt->tm_hour;
        int mes = lt->tm_mon + 1;

        std::string estacion_año;
        if (mes == 12 || mes <= 2) estacion_año = "invierno";
        else if (mes >= 3 && mes <= 5) estacion_año = "primavera";
        else if (mes >= 6 && mes <= 8) estacion_año = "verano";
        else estacion_año = "otoño";

        // Radiación solar base (día-noche)
        float rad_dia = 0.0f;
        if (hora >= 6 && hora <= 18) {
            rad_dia = std::max(0.0f, 1.0f - std::abs(12 - hora) / 6.0f);
        }

        {
            std::lock_guard<std::mutex> lock(motor.mtx);
            motor.estacion_año = estacion_año;
            motor.presion = clamp(motor.presion + pres_delta(gen), 950.0f, 1050.0f);
            motor.nubosidad = clamp(motor.nubosidad + nub_delta(gen), 0.0f, 100.0f);
            motor.radiacion = clamp(motor.radiacion + solar_delta(gen), 0.0f, 1200.0f * rad_dia);

            // Eventos extremos poco frecuentes
            motor.ola_calor = (evento(gen) < 0.01);
            motor.tormenta = (evento(gen) < 0.02);
        }

        std::this_thread::sleep_for(std::chrono::minutes(1));
    }
}

// ===================== HILO DE ESTACIÓN =====================
void station_thread(Estacion estacion) {
    mosquitto_lib_init();
    std::string client_id = "publisher_" + std::to_string(estacion.id);
    struct mosquitto *mosq = mosquitto_new(client_id.c_str(), true, nullptr);
    if (!mosq) return;
    int rc = mosquitto_connect(mosq, "localhost", 1883, 60);
    while (rc != MOSQ_ERR_SUCCESS) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        rc = mosquitto_reconnect(mosq);
    }

    std::uniform_real_distribution<> temp_init(15.0, 25.0), hum_init(40.0, 70.0);
    float temperatura = temp_init(gen);
    float humedad = hum_init(gen);
    float viento = 5.0f, direccion_viento = 180.0f, precipitacion = 0.0f;
    float tendencia_temp = temperatura;

    std::normal_distribution<> temp_delta(0.0, 0.3), hum_delta(0.0, 0.5);
    std::normal_distribution<> viento_delta(0.0, 0.5), dir_delta(0.0, 2.0);

    std::string topic = "sensores/clima/" + estacion.nombre;

    while (true) {
        auto now = std::chrono::system_clock::now();
        std::time_t now_c = std::chrono::system_clock::to_time_t(now);

        EstadoGlobal copia = motor.snapshot();

        // Ajustes por región
        float ajuste_temp = 0.0f, ajuste_hum = 0.0f;
        if (estacion.region == "Litoral Urbano") { ajuste_hum = +10.0f; }
        else if (estacion.region == "Interior Seco") { ajuste_temp = +3.0f; ajuste_hum = -10.0f; }
        else if (estacion.region == "Alta Montaña") { ajuste_temp = -7.0f; ajuste_hum = +5.0f; }
        else if (estacion.region == "Pre-Pirineo") { ajuste_temp = -2.0f; ajuste_hum = +5.0f; }

        // Temperatura
        tendencia_temp += temp_delta(gen);
        temperatura = clamp(tendencia_temp + ajuste_temp + (copia.ola_calor ? 5.0f : 0.0f), 5.0f, 40.0f);

        // Humedad
        humedad = clamp(humedad + hum_delta(gen) + ajuste_hum, 20.0f, 95.0f);

        // Viento
        viento = clamp(viento + viento_delta(gen), 0.0f, 20.0f);
        direccion_viento += dir_delta(gen);
        if (direccion_viento < 0.0f) direccion_viento += 360.0f;
        if (direccion_viento > 360.0f) direccion_viento -= 360.0f;

        // Precipitación
        float rain_prob = (copia.tormenta ? 0.7f : 0.2f) + copia.nubosidad / 200.0f;
        if (std::uniform_real_distribution<>(0.0, 1.0)(gen) < rain_prob) {
            precipitacion = (copia.tormenta ? 5.0f : 1.0f);
        } else {
            precipitacion = 0.0f;
        }

        // Sensación térmica
        float sensacion_termica = temperatura;
        if (temperatura < 10.0f && viento > 3.0f) {
            sensacion_termica = 13.12f + 0.6215f * temperatura - 11.37f * pow(viento, 0.16f)
                                + 0.3965f * temperatura * pow(viento, 0.16f);
        } else if (temperatura > 26.0f && humedad > 40.0f) {
            sensacion_termica = -8.784695f + 1.61139411f * temperatura + 2.338549f * humedad
                                - 0.14611605f * temperatura * humedad
                                - 0.012308094f * pow(temperatura, 2)
                                - 0.016424828f * pow(humedad, 2)
                                + 0.002211732f * pow(temperatura, 2) * humedad
                                + 0.00072546f * temperatura * pow(humedad, 2)
                                - 0.000003582f * pow(temperatura, 2) * pow(humedad, 2);
        }

        // UV Index
        float uv_index = clamp((copia.radiacion / 1200.0f) * 11.0f, 0.0f, 11.0f);

        // Formato y envío
        char timestamp[32];
        std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now_c));
        char mensaje[1024];
        snprintf(mensaje, sizeof(mensaje),
            "{\"id\":%d,\"nombre\":\"%s\",\"region\":\"%s\",\"timestamp\":\"%s\","
            "\"estacion_año\":\"%s\",\"nubosidad\":%.1f,\"uv_index\":%.1f,"
            "\"humedad\":%.1f,\"temperatura\":%.1f,\"sensacion_termica\":%.1f,"
            "\"presion\":%.1f,\"viento\":%.1f,\"direccion_viento\":%.1f,"
            "\"precipitacion\":%.1f,\"radiacion_solar\":%.1f}",
            estacion.id, estacion.nombre.c_str(), estacion.region.c_str(), timestamp,
            copia.estacion_año.c_str(), copia.nubosidad, uv_index,
            humedad, temperatura, sensacion_termica,
            copia.presion, viento, direccion_viento, precipitacion, copia.radiacion);

        int ret = mosquitto_publish(mosq, nullptr, topic.c_str(), strlen(mensaje), mensaje, 0, false);
        if (ret != MOSQ_ERR_SUCCESS) {
            std::cerr << "Error publicando desde estación " << estacion.nombre << std::endl;
        } else {
            std::cout << estacion.nombre << " publicó en " << topic << ": " << mensaje << std::endl;
        }

        std::this_thread::sleep_for(std::chrono::minutes(1));
    }
}

// ===================== MAIN =====================
int main() {
    // Inicializar motor global con valores medios
    motor.presion = 1015.0f;
    motor.nubosidad = 30.0f;
    motor.radiacion = 500.0f;
    motor.ola_calor = false;
    motor.tormenta = false;

    std::vector<Estacion> estaciones = {
        {1, "Barcelona", "Litoral Urbano"},
        {2, "Tarragona", "Litoral Sur"},
        {3, "Girona", "Pre-Pirineo"},
        {4, "Lleida", "Interior Seco"},
        {5, "Pirineos", "Alta Montaña"}
    };

    std::thread motor_thread(motor_global_thread);
    std::vector<std::thread> threads;
    for (auto &est : estaciones) {
        threads.emplace_back(station_thread, est);
    }

    motor_thread.join();
    for (auto &t : threads) {
        t.join();
    }

    return 0;
}
