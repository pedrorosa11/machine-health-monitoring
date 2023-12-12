#include <iostream>
#include <sstream>
#include <chrono>
#include <thread>
#include <deque>
#include <map>
#include <boost/asio.hpp>
#include "json.hpp"
#include "mqtt/client.h"
#include "mqtt/connect_options.h"

namespace asio = boost::asio;
using namespace std;
using asio::ip::tcp;

const int QOS = 1;
const string SERVER_ADDRESS = "tcp://localhost:1883";
const string GRAPHITE_HOST = "127.0.0.1";
const int GRAPHITE_PORT = 2003;
const int TIME_FROM_SENSOR_MONITOR_IN_SECONDS = 1;
const double INACTIVITY_THRESHOLD = 10.0;
const double MEMORY_USAGE_THRESHOLD = 90.0;

map<pair<string, string>, chrono::steady_clock::time_point> lastSensorActivity;
chrono::steady_clock::time_point startTime = chrono::steady_clock::now();
map<pair<string, string>, deque<double>> lastSensorReadings;

// Function declarations
string TimestampToUnix(const string &timestamp);
string UnixToTimestamp(const time_t &timestamp);
void PublishToGraphite(const string &metric);
void PostMetric(const string &machineId, const string &uniqueSensorId, const string &timestampStr, const double value);
double FindSensorFrequency();
void UpdateSensorActivity(const string &machineId, const string &sensorId);
void InactivityAlarmProcessing();
vector<string> Split(const string &str, char delim);

class MQTTCallback : public virtual mqtt::callback
{
public:
    void message_arrived(mqtt::const_message_ptr msg) override
    {
        auto jsonPayload = nlohmann::json::parse(msg->get_payload());
        string topic = msg->get_topic();
        auto topicParts = Split(topic, '/');
        string machineId = topicParts[2];
        string sensorId = topicParts[3];
        string timestamp = jsonPayload["timestamp"];
        double value = jsonPayload["value"];

        PostMetric(machineId, sensorId, timestamp, value);
        UpdateSensorActivity(machineId, sensorId);
    }
};

int main(int argc, char *argv[])
{
    string clientId = "client_id";
    mqtt::async_client client(SERVER_ADDRESS, clientId);
    MQTTCallback callback;
    client.set_callback(callback);
    mqtt::connect_options connOpts;
    connOpts.set_keep_alive_interval(20);
    connOpts.set_clean_session(true);

    try
    {
        client.connect(connOpts)->wait();
        client.subscribe("/sensors/#", QOS);
        cout << "Subscribed" << endl << endl;
    }
    catch (mqtt::exception &e)
    {
        cerr << "Error: " << e.what() << endl;
        return EXIT_FAILURE;
    }

    while (true)
    {
        InactivityAlarmProcessing();
        this_thread::sleep_for(chrono::seconds(1));
    }

    return EXIT_SUCCESS;
}

string TimestampToUnix(const string &timestamp)
{
    tm timeStruct = {};
    istringstream ss(timestamp);
    ss >> get_time(&timeStruct, "%Y-%m-%dT%H:%M:%S");
    time_t timeStamp = mktime(&timeStruct);
    return to_string(timeStamp);
}

string UnixToTimestamp(const time_t &timestamp)
{
    tm *timeStruct = localtime(&timestamp);
    char buffer[32];
    strftime(buffer, 32, "%Y-%m-%dT%H:%M:%S", timeStruct);
    return string(buffer);
}

void PublishToGraphite(const string &metric)
{
    try
    {
        asio::io_context ioContext;
        tcp::socket socket(ioContext);
        tcp::resolver resolver(ioContext);
        asio::connect(socket, resolver.resolve(GRAPHITE_HOST, to_string(GRAPHITE_PORT)));
        asio::write(socket, asio::buffer(metric));
    }
    catch (const exception &e)
    {
        cerr << "Error in PublishToGraphite: " << e.what() << endl;
    }
}

void PostMetric(const string &machineId, const string &sensorId, const string &timestampStr, const double value)
{
    try
    {
        string metricPath = machineId + "." + sensorId;
        string graphiteMetric = metricPath + " " + to_string(value) + " " + TimestampToUnix(timestampStr) + "\n";
        PublishToGraphite(graphiteMetric);
        string printGraphiteMetric = metricPath + " " + to_string(value) + " " + timestampStr;
        cout << "Posted: " << printGraphiteMetric << endl << endl;
    }
    catch (const exception &e)
    {
        cerr << "Error in PostMetric: " << e.what() << endl;
    }
}

vector<string> Split(const string &str, char delim)
{
    vector<string> tokens;
    string token;
    istringstream tokenStream(str);
    while (getline(tokenStream, token, delim))
    {
        tokens.push_back(token);
    }
    return tokens;
}

double FindSensorFrequency()
{
    chrono::steady_clock::time_point endTime = chrono::steady_clock::now();
    chrono::duration<double> elapsedTime = endTime - startTime;
    startTime = endTime;
    return elapsedTime.count() * 10 * TIME_FROM_SENSOR_MONITOR_IN_SECONDS;
}

void InactivityAlarmProcessing()
{
    time_t currentTime = time(nullptr);
    double frequency = FindSensorFrequency();

    for (auto &activity : lastSensorActivity)
    {
        auto machineSensorPair = activity.first;
        auto lastActivityTime = activity.second;
        auto machineId = machineSensorPair.first;
        auto sensorId = machineSensorPair.second;

        auto elapsedTime = chrono::steady_clock::now() - lastActivityTime;
        auto elapsedTimeInSeconds = chrono::duration_cast<chrono::seconds>(elapsedTime).count();

        if (elapsedTimeInSeconds >= frequency)
        {
            string alarmPath = machineId + ".alarms.inactive." + sensorId;
            string message = alarmPath + " 1 " + UnixToTimestamp(currentTime) + "\n";
            PostMetric(machineId, "alarms.inactive." + sensorId, UnixToTimestamp(currentTime), 1);
        }
    }
}

void UpdateSensorActivity(const string &machineId, const string &sensorId)
{
    lastSensorActivity[{machineId, sensorId}] = chrono::steady_clock::now();
}