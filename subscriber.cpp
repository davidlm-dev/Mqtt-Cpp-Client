    #include <iostream>
#include <mosquitto.h>


void on_message(struct mosquitto *mosq, void *userdata, const struct mosquitto_message *message) {
    std::cout << "Tópico: " << message->topic << std::endl;
    std::cout << "Datos: " << (char*)message->payload << std::endl;
    std::cout << "-----------------------------" << std::endl;
}


int main() {
    mosquitto_lib_init();
    struct mosquitto *mosq = mosquitto_new("subscriber", true, nullptr);
    if (!mosq) {
        std::cerr << "Error creando el cliente Mosquitto" << std::endl;
        return 1;
    }
    mosquitto_message_callback_set(mosq, on_message);
    if (mosquitto_connect(mosq, "localhost", 1883, 60) != MOSQ_ERR_SUCCESS) {
        std::cerr << "No se pudo conectar al broker" << std::endl;
        return 1;
    }
    // Suscribirse a todos los tópicos de estaciones
    mosquitto_subscribe(mosq, nullptr, "sensores/clima/#", 0);
    std::cout << "Esperando mensajes de todas las estaciones... (Ctrl+C para salir)" << std::endl;
    while (true) {
        mosquitto_loop(mosq, -1, 1);
    }
    mosquitto_disconnect(mosq);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    return 0;
}
