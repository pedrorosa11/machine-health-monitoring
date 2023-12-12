#include <iostream>
#include <cstdlib>
#include <chrono>
#include <ctime>
#include <thread>
#include <unistd.h>
#include "json.hpp"
#include "mqtt/client.h"
#include <iomanip>
#include <sstream>
#include <fstream>
#include <vector>
#include <sys/sysinfo.h>
#include "mqtt/async_client.h"

// Definições de constantes
#define MONITOR_TOPIC "/sensor_monitors"
#define QOS 1
#define SERVER_ADDRESS "tcp://localhost:1883"
#define GRAPHITE_HOST "127.0.0.1"
#define GRAPHITE_PORT 2003
#define TIME_FROM_SENSOR_MONITOR_IN_SECONDS 1

using namespace std;

// Estrutura para representar informações de um sensor
struct Sensor {
    string sensor_id;
    string data_type;
    long data_interval;
};

// Função para criar a carga útil JSON
nlohmann::json createJsonPayload(const std::string& machineId, const std::vector<nlohmann::json>& sensors) {
    nlohmann::json payload;
    payload["machine_id"] = machineId;
    payload["sensors"] = sensors;
    return payload;
}

// Função para calcular a porcentagem de uso da CPU
double cpu_percentage() {
    std::ifstream stat_file("/proc/stat");
    std::string line;
    
    // Lê a primeira linha do arquivo /proc/stat para obter as informações de uso da CPU
    std::getline(stat_file, line);

    unsigned long user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice;
    sscanf(line.c_str(), "cpu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
           &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal, &guest, &guest_nice);

    unsigned long total_time = user + nice + system + idle + iowait + irq + softirq + steal;
    
    // Aguarda por 1 segundo
    sleep(1);

    // Lê o arquivo /proc/stat novamente para obter as informações de uso atualizadas
    stat_file.seekg(0);
    std::getline(stat_file, line);

    unsigned long user_new, nice_new, system_new, idle_new, iowait_new, irq_new, softirq_new, steal_new, guest_new, guest_nice_new;
    sscanf(line.c_str(), "cpu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
           &user_new, &nice_new, &system_new, &idle_new, &iowait_new, &irq_new, &softirq_new, &steal_new, &guest_new, &guest_nice_new);

    unsigned long elapsed_time = user_new + nice_new + system_new + idle_new + iowait_new + irq_new + softirq_new + steal_new - total_time;

    double cpu_percentage = (double)elapsed_time / sysconf(_SC_CLK_TCK);
    return cpu_percentage;
}

// Função para calcular a porcentagem de uso da memória
double memory_used() {
    std::ifstream meminfo_file("/proc/meminfo");
    if (!meminfo_file) {
        std::cerr << "Falha ao abrir o arquivo /proc/meminfo." << std::endl;
        return -1;
    }

    std::string line;
    std::string mem_total_str;
    std::string mem_available_str;

    // Lê cada linha do arquivo /proc/meminfo
    while (std::getline(meminfo_file, line)) {
        std::istringstream iss(line);
        std::string key;
        std::string value;

        iss >> key >> value;

        // Verifica se a linha contém "MemTotal:" ou "MemAvailable:"
        // Se sim, armazena o valor correspondente nas variáveis apropriadas
        if (key == "MemTotal:") {
            mem_total_str = value;
        } else if (key == "MemAvailable:") {
            mem_available_str = value;
        }

        // Se ambas mem_total_str e mem_available_str não estiverem vazias,
        // significa que obtivemos as informações de memória necessárias
        // Sai do loop para interromper a leitura de mais linhas
        if (!mem_total_str.empty() && !mem_available_str.empty()) {
            break;
        }
    }

    if (mem_total_str.empty() || mem_available_str.empty()) {
        std::cerr << "Falha ao obter informações de memória do arquivo /proc/meminfo." << std::endl;
        return -1;
    }

    // Converte os valores de memória obtidos de string para double
    double mem_total = std::stod(mem_total_str);
    double mem_available = std::stod(mem_available_str);

    // Calcula a porcentagem de uso de memória subtraindo a memória disponível da memória total,
    // dividindo pela memória total e multiplicando por 100
    double mem_usage_percentage = (mem_total - mem_available) / mem_total * 100.0;

    return mem_usage_percentage;
}

// Função para publicar monitores de sensor no broker MQTT
void publishToMqttINMSG(mqtt::client& client, nlohmann::json j, std::string machineId, int freqcpu, int freqmem, int freq) {
    while(true) {
        std::vector<nlohmann::json> sensors;
        std::string sensor_monitors = "/sensor_monitors";
        mqtt::message msg_mem(sensor_monitors, j.dump(), QOS, false);
        j["machine_id"] = machineId;
        nlohmann::json sensor1, sensor2;
        sensor1["sensor_id"] = {"MEM_USED"};
        sensor1["data_type"] = {"double"};
        sensor1["data_interval"] = {std::to_string(freqmem)};
        sensors.push_back(sensor1);

        sensor2["sensor_id"] = {"CPU_USED"};
        sensor2["data_type"] = {"double"};
        sensor2["data_interval"] = {std::to_string(freqcpu)};
        sensors.push_back(sensor2);

        nlohmann::json payload = createJsonPayload(machineId, sensors);
        client.publish(msg_mem);
        std::clog << "Mensagem Publicada\nTópico: " << sensor_monitors << " - Mensagem: " << payload.dump() << std::endl;
        std::cout << "\n";
        std::this_thread::sleep_for(std::chrono::seconds(freq));
    }
}

// Função para publicar o uso da CPU no broker MQTT
void publishToMqttCPU(mqtt::client& client, nlohmann::json j, std::string machineId, int freq) {
    while(true) {
        auto now = std::chrono::system_clock::now();
        std::time_t now_c = std::chrono::system_clock::to_time_t(now);
        std::tm* now_tm = std::localtime(&now_c);
        std::stringstream ss;
        ss << std::put_time(now_tm, "%FT%TZ");
        std::string timestamp = ss.str();

        double cpu_used = cpu_percentage();

        std::string topic_cpuused = "/sensors/" + machineId + "/CPU_USED";
        mqtt::message msg_cpu(topic_cpuused, j.dump(), QOS, false);
        j["timestamp"] = timestamp;
        j["value"] = cpu_used;
        client.publish(msg_cpu);
        std::clog << "Mensagem Publicada\nTópico: " << topic_cpuused << " - Mensagem: " << j.dump() << std::endl;
        std::cout << "\n";
        std::this_thread::sleep_for(std::chrono::seconds(freq - 1));
    }
}

// Função para publicar o uso da memória no broker MQTT
void publishToMqttMEM(mqtt::client& client, nlohmann::json j, std::string machineId, int freq) {
    while(true) {
        auto now = std::chrono::system_clock::now();
        std::time_t now_c = std::chrono::system_clock::to_time_t(now);
        std::tm* now_tm = std::localtime(&now_c);
        std::stringstream ss;
        ss << std::put_time(now_tm, "%FT%TZ");
        std::string timestamp = ss.str();

        double mem_used = memory_used();

        std::string topic_memused = "/sensors/" + machineId + "/MEM_USED";
        mqtt::message msg_mem(topic_memused, j.dump(), QOS, false);
        j["timestamp"] = timestamp;
        j["value"] = mem_used;
        client.publish(msg_mem);
        std::clog << "Mensagem Publicada\nTópico: " << topic_memused << " - Mensagem: " << j.dump() << std::endl;
        std::cout << "\n";

        std::this_thread::sleep_for(std::chrono::seconds(freq));
    }
}

int main(int argc, char *argv[]) {
    std::string clientId = "sensor-monitor";
    mqtt::client client(SERVER_ADDRESS, clientId);

    mqtt::connect_options connOpts;
    connOpts.set_keep_alive_interval(20);
    connOpts.set_clean_session(true);

    try {
        client.connect(connOpts);
    } catch (mqtt::exception& e) {
        std::cerr << "Erro: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    std::clog << "conectado ao broker" << std::endl;

    char hostname[1024];
    gethostname(hostname, 1024);
    std::string machineId(hostname);

    nlohmann::json j;
    int cpufreq, memfreq, msgfreq;
    std::clog << "Informe a frequência de leitura da CPU (s): ";
    std::cin >> cpufreq;
    std::clog << std::endl;
    std::clog << "Informe a frequência de leitura da MEMÓRIA (s): ";
    std::cin >> memfreq;
    std::clog << std::endl;
    msgfreq = 10;
    std::clog << "Frequência de Mensagens dos Sensores (s): " << msgfreq ;
    std::clog << std::endl;
    sleep(2);

    // Inicia threads para publicação de monitores e leituras de CPU e MEM
    std::thread inMessafe(publishToMqttINMSG, std::ref(client), j, machineId, cpufreq, memfreq, msgfreq);
    std::thread cpuThread(publishToMqttCPU, std::ref(client), j, machineId, cpufreq);
    std::thread memoryThread(publishToMqttMEM, std::ref(client), j, machineId, memfreq);

    // Aguarda até que as threads terminem
    inMessafe.join();
    cpuThread.join();
    memoryThread.join();

    return EXIT_SUCCESS;
}