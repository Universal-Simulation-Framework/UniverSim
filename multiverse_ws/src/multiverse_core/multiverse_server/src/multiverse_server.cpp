// Copyright (c) 2023, Hoang Giang Nguyen - Institute for Artificial Intelligence, University Bremen

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <ros/ros.h>

#include <chrono>
#include <csignal>
#include <iostream>
#include <jsoncpp/json/json.h>
#include <jsoncpp/json/reader.h>
#include <mutex>
#include <thread>
#include <zmq.hpp>

using namespace std::chrono_literals;

enum class EAttribute : unsigned char
{
    Position,
    Quaternion,
    RelativeVelocity,
    JointRvalue,
    JointTvalue,
    JointPosition,
    JointQuaternion,
    Force,
    Torque
};

enum class EFlag : unsigned char
{
    RequestSendMetaData,
    BindObjects,
    ResponseReceiveMetaData,
    RequestSendData,
    BindSendData,
    BindReceiveData,
    ResponseReceiveData,
};

std::map<std::string, std::pair<EAttribute, std::vector<double>>> attribute_map =
    {
        {"position", {EAttribute::Position, {0.0, 0.0, 0.0}}},
        {"quaternion", {EAttribute::Quaternion, {1.0, 0.0, 0.0, 0.0}}},
        {"relative_velocity", {EAttribute::RelativeVelocity, {0.0, 0.0, 0.0, 0.0, 0.0, 0.0}}},
        {"joint_rvalue", {EAttribute::JointRvalue, {0.0}}},
        {"joint_tvalue", {EAttribute::JointTvalue, {0.0}}},
        {"joint_position", {EAttribute::JointPosition, {0.0, 0.0, 0.0}}},
        {"joint_quaternion", {EAttribute::JointQuaternion, {1.0, 0.0, 0.0, 0.0}}},
        {"force", {EAttribute::Force, {0.0, 0.0, 0.0}}},
        {"torque", {EAttribute::Torque, {0.0, 0.0, 0.0}}}};

std::map<std::string, double> unit_scale =
    {
        {"s", 1.0},
        {"ms", 0.001},
        {"us", 0.00001},
        {"m", 1.0},
        {"cm", 0.01},
        {"rad", 1.0},
        {"deg", M_PI / 180.0},
        {"N", 1.0}};

std::map<EAttribute, std::map<std::string, std::vector<double>>> handedness_scale =
    {
        {EAttribute::Position,
         {{"rhs", {1.0, 1.0, 1.0}},
          {"lhs", {1.0, -1.0, 1.0}}}},
        {EAttribute::Quaternion,
         {{"rhs", {1.0, 1.0, 1.0, 1.0}},
          {"lhs", {-1.0, 1.0, -1.0, 1.0}}}},
        {EAttribute::RelativeVelocity,
         {{"rhs", {1.0, 1.0, 1.0, 1.0, 1.0, 1.0}},
          {"lhs", {1.0, 1.0, 1.0, 1.0, 1.0, 1.0}}}},
        {EAttribute::JointRvalue,
         {{"rhs", {1.0}},
          {"lhs", {-1.0}}}},
        {EAttribute::JointTvalue,
         {{"rhs", {1.0}},
          {"lhs", {-1.0}}}},
        {EAttribute::JointPosition,
         {{"rhs", {1.0, 1.0, 1.0}},
          {"lhs", {1.0, -1.0, 1.0}}}},
        {EAttribute::JointQuaternion,
         {{"rhs", {1.0, 1.0, 1.0, 1.0}},
          {"lhs", {1.0, 1.0, -1.0, 1.0}}}},
        {EAttribute::Force,
         {{"rhs", {1.0, 1.0, 1.0}},
          {"lhs", {1.0, -1.0, 1.0}}}},
        {EAttribute::Torque,
         {{"rhs", {1.0, 1.0, 1.0}},
          {"lhs", {1.0, -1.0, 1.0}}}}};

std::vector<std::thread> workers;

std::mutex mtx;

std::map<std::string, std::map<std::string, std::map<std::string, std::pair<std::vector<double>, bool>>>> worlds;

std::map<std::string, std::map<std::string, std::map<std::string, std::map<std::string, std::vector<double>>>>> send_efforts;

bool should_shut_down = false;

std::map<std::string, bool> sockets_need_clean_up;

zmq::context_t context{1};

static double get_time_now()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count() / 1000000.0;
}

class MultiverseServer
{
public:
    MultiverseServer(const std::string &socket_addr) : socket_addr(socket_addr)
    {
        socket_server = zmq::socket_t(context, zmq::socket_type::rep);
        socket_server.bind(socket_addr);
        sockets_need_clean_up[socket_addr] = false;
        ROS_INFO("[Server] Bind to socket [%s].", socket_addr.c_str());
    }

    ~MultiverseServer()
    {
        ROS_INFO("[Server] Close socket [%s].", socket_addr.c_str());

        if (send_buffer != nullptr)
        {
            free(send_buffer);
        }

        if (receive_buffer != nullptr)
        {
            free(receive_buffer);
        }

        sockets_need_clean_up[socket_addr] = false;
    }

public:
    void start()
    {
        while (!should_shut_down)
        {
            switch (flag)
            {
            case EFlag::RequestSendMetaData:
                request_send_meta_data();
                message_str = message.to_string();
                if (message_str[0] != '{' ||
                    message_str[0] == '{' && message_str[1] == '}' ||
                    message_str.empty() ||
                    !reader.parse(message_str, send_meta_data_json))
                {
                    flag = EFlag::BindReceiveData;
                }
                else
                {
                    sockets_need_clean_up[socket_addr] = false;
                    flag = EFlag::BindObjects;
                }
                break;

            case EFlag::BindObjects:
                init_receive_meta_data();

                mtx.lock();
                bind_send_objects();
                validate_receive_meta_data();
                mtx.unlock();

                wait_for_objects();

                mtx.lock();
                bind_receive_objects();
                mtx.unlock();

                flag = EFlag::ResponseReceiveMetaData;

            case EFlag::ResponseReceiveMetaData:
                response_meta_data();
                if (send_buffer_size == 1 && receive_buffer_size == 1)
                {
                    flag = EFlag::RequestSendMetaData;
                }
                else
                {
                    flag = EFlag::RequestSendData;
                    sockets_need_clean_up[socket_addr] = true;
                }
                break;

            case EFlag::RequestSendData:
                request_send_data();
                if (message.to_string()[0] == '{')
                {
                    message_str = message.to_string();
                    if (message_str[1] == '}')
                    {
                        send_data_vec.clear();
                        receive_data_vec.clear();
                        flag = EFlag::ResponseReceiveMetaData;
                    }
                    else if (reader.parse(message_str, send_meta_data_json) && !send_meta_data_json.empty())
                    {
                        send_data_vec.clear();
                        receive_data_vec.clear();
                        flag = EFlag::BindObjects;
                    }
                    else if (isnan(send_buffer[0]))
                    {
                        ROS_WARN("Received [%s] from [%s].", message_str.c_str(), socket_addr.c_str());
                        flag = EFlag::BindReceiveData;
                    }
                    else
                    {
                        flag = EFlag::BindSendData;
                    }
                }
                else
                {
                    flag = EFlag::BindSendData;
                }
                break;

            case EFlag::BindSendData:
                mtx.lock();
                for (size_t i = 0; i < send_buffer_size - 1; i++)
                {
                    *send_data_vec[i].first = send_buffer[i + 1] * send_data_vec[i].second;
                }
                mtx.unlock();
                flag = EFlag::BindReceiveData;
                break;

            case EFlag::BindReceiveData:
                wait_for_receive_data();

                mtx.lock();
                compute_cumulative_data();
                mtx.unlock();

                for (size_t i = 0; i < receive_buffer_size - 1; i++)
                {
                    receive_buffer[i + 1] = *receive_data_vec[i].first * receive_data_vec[i].second;
                }
                flag = EFlag::ResponseReceiveData;

            case EFlag::ResponseReceiveData:
                response_receive_data();
                flag = EFlag::RequestSendData;
                break;

            default:
                break;
            }
        }

        if (sockets_need_clean_up[socket_addr])
        {
            if (flag != EFlag::RequestSendData && flag != EFlag::RequestSendMetaData)
            {
                response_receive_data();
            }

            ROS_INFO("[Server] Unbind from socket %s.", socket_addr.c_str());
            socket_server.unbind(socket_addr);
        }
    }

private:
    void request_send_meta_data()
    {
        send_buffer_size = 1;
        receive_buffer_size = 1;

        send_buffer = (double *)calloc(send_buffer_size, sizeof(double));
        receive_buffer = (double *)calloc(receive_buffer_size, sizeof(double));

        is_receive_data_sent = false;

        // Receive JSON string over ZMQ
        try
        {
            sockets_need_clean_up[socket_addr] = false;
            socket_server.recv(message, zmq::recv_flags::none);
            sockets_need_clean_up[socket_addr] = true;
        }
        catch (const zmq::error_t &e)
        {
            should_shut_down = true;
            ROS_INFO("[Server] %s, socket %s prepares to close.", e.what(), socket_addr.c_str());
        }
    }

    void init_receive_meta_data()
    {
        ROS_INFO("[Server] Receive meta data from socket %s:\n%s", socket_addr.c_str(), send_meta_data_json.toStyledString().c_str());

        world_name = send_meta_data_json.isMember("world") ? send_meta_data_json["world"].asString() : "world";
        const std::string length_unit = send_meta_data_json.isMember("length_unit") ? send_meta_data_json["length_unit"].asString() : "m";
        const std::string angle_unit = send_meta_data_json.isMember("angle_unit") ? send_meta_data_json["angle_unit"].asString() : "rad";
        const std::string handedness = send_meta_data_json.isMember("handedness") ? send_meta_data_json["handedness"].asString() : "rhs";
        const std::string force_unit = send_meta_data_json.isMember("force_unit") ? send_meta_data_json["force_unit"].asString() : "N";
        const std::string time_unit = send_meta_data_json.isMember("time_unit") ? send_meta_data_json["time_unit"].asString() : "s";

        for (const std::pair<const std::string, std::pair<EAttribute, std::vector<double>>> &attribute : attribute_map)
        {
            conversion_map.emplace(attribute.second);
        }

        std::for_each(conversion_map[EAttribute::Position].begin(), conversion_map[EAttribute::Position].end(),
                      [length_unit](double &position)
                      { position = unit_scale[length_unit]; });

        std::for_each(conversion_map[EAttribute::Quaternion].begin(), conversion_map[EAttribute::Quaternion].end(),
                      [](double &quaternion)
                      { quaternion = 1.0; });

        std::for_each(conversion_map[EAttribute::JointRvalue].begin(), conversion_map[EAttribute::JointRvalue].end(),
                      [angle_unit](double &joint_rvalue)
                      { joint_rvalue = unit_scale[angle_unit]; });

        std::for_each(conversion_map[EAttribute::JointTvalue].begin(), conversion_map[EAttribute::JointTvalue].end(),
                      [length_unit](double &joint_tvalue)
                      { joint_tvalue = unit_scale[length_unit]; });

        std::for_each(conversion_map[EAttribute::JointPosition].begin(), conversion_map[EAttribute::JointPosition].end(),
                      [length_unit](double &joint_position)
                      { joint_position = unit_scale[length_unit]; });

        std::for_each(conversion_map[EAttribute::JointQuaternion].begin(), conversion_map[EAttribute::JointQuaternion].end(),
                      [](double &joint_quaternion)
                      { joint_quaternion = 1.0; });

        std::for_each(conversion_map[EAttribute::Force].begin(), conversion_map[EAttribute::Force].end(),
                      [force_unit](double &force)
                      { force = unit_scale[force_unit]; });

        std::for_each(conversion_map[EAttribute::Torque].begin(), conversion_map[EAttribute::Torque].end(),
                      [force_unit, length_unit](double &torque)
                      { torque = unit_scale[force_unit] * unit_scale[length_unit]; });

        for (size_t i = 0; i < 3; i++)
        {
            conversion_map[EAttribute::RelativeVelocity][i] = unit_scale[length_unit] / unit_scale[time_unit];
        }
        for (size_t i = 3; i < 6; i++)
        {
            conversion_map[EAttribute::RelativeVelocity][i] = unit_scale[angle_unit] / unit_scale[time_unit];
        }

        for (std::pair<const EAttribute, std::vector<double>> &conversion_scale : conversion_map)
        {
            std::vector<double>::iterator conversion_scale_it = conversion_scale.second.begin();
            std::vector<double>::iterator handedness_scale_it = handedness_scale[conversion_scale.first][handedness].begin();
            for (size_t i = 0; i < conversion_scale.second.size(); i++)
            {
                *(conversion_scale_it++) *= *(handedness_scale_it++);
            }
        }

        receive_meta_data_json.clear();
        receive_meta_data_json["world"] = world_name;
        receive_meta_data_json["time"] = get_time_now() * unit_scale[time_unit];
        receive_meta_data_json["angle_unit"] = angle_unit;
        receive_meta_data_json["force_unit"] = force_unit;
        receive_meta_data_json["time_unit"] = time_unit;
        receive_meta_data_json["handedness"] = handedness;
    }

    void bind_send_objects()
    {
        send_objects_json = send_meta_data_json["send"];
        std::map<std::string, std::map<std::string, std::pair<std::vector<double>, bool>>> &objects = worlds[world_name];
        for (const std::string &object_name : send_objects_json.getMemberNames())
        {
            if (objects.count(object_name) == 0)
            {
                objects[object_name] = {};
                objects[object_name]["force"] = {attribute_map["force"].second, false};
                objects[object_name]["torque"] = {attribute_map["torque"].second, false};
            }

            for (const Json::Value &attribute_json : send_objects_json[object_name])
            {
                const std::string attribute_name = attribute_json.asString();
                if (objects[object_name].count(attribute_name) == 0)
                {
                    objects[object_name][attribute_name] = {attribute_map[attribute_name].second, false};

                    for (size_t i = 0; i < objects[object_name][attribute_name].first.size(); i++)
                    {
                        double *data = &objects[object_name][attribute_name].first[i];
                        const double conversion = conversion_map[attribute_map[attribute_name].first][i];
                        send_data_vec.emplace_back(data, conversion);
                        receive_meta_data_json["send"][object_name][attribute_name].append(std::numeric_limits<double>::quiet_NaN());
                    }
                }
                else if (strcmp(attribute_name.c_str(), "force") == 0 || strcmp(attribute_name.c_str(), "torque") == 0)
                {
                    send_efforts[world_name][object_name][socket_addr][attribute_name] = attribute_map[attribute_name].second;

                    for (size_t i = 0; i < attribute_map[attribute_name].second.size(); i++)
                    {
                        double *data = &send_efforts[world_name][object_name][socket_addr][attribute_name][i];
                        const double conversion = conversion_map[attribute_map[attribute_name].first][i];
                        send_data_vec.emplace_back(data, conversion);
                        receive_meta_data_json["send"][object_name][attribute_name].append(*data * conversion);
                    }
                }
                else
                {
                    ROS_INFO("[Server] Continue state [%s - %s] on socket %s", object_name.c_str(), attribute_name.c_str(), socket_addr.c_str());
                    continue_state = true;
                    objects[object_name][attribute_name].second = false;

                    for (size_t i = 0; i < objects[object_name][attribute_name].first.size(); i++)
                    {
                        double *data = &objects[object_name][attribute_name].first[i];
                        const double conversion = conversion_map[attribute_map[attribute_name].first][i];
                        send_data_vec.emplace_back(data, conversion);
                        receive_meta_data_json["send"][object_name][attribute_name].append(*data * conversion);
                    }
                }
            }
        }
    }

    void validate_receive_meta_data()
    {
        receive_objects_json = send_meta_data_json["receive"];

        if (receive_objects_json.isMember("") &&
            std::find(receive_objects_json[""].begin(), receive_objects_json[""].end(), "") != receive_objects_json[""].end())
        {
            receive_objects_json = {};
            for (const std::pair<std::string, std::map<std::string, std::pair<std::vector<double>, bool>>> &object : worlds[world_name])
            {
                for (const std::pair<std::string, std::pair<std::vector<double>, bool>> &attribute : object.second)
                {
                    if ((strcmp(attribute.first.c_str(), "force") != 0 && strcmp(attribute.first.c_str(), "torque") != 0 ||
                         attribute.second.first.size() > 3))
                    {
                        receive_objects_json[object.first].append(attribute.first);
                    }
                }
            }
            return;
        }

        for (const std::string &object_name : send_meta_data_json["receive"].getMemberNames())
        {
            if (!object_name.empty())
            {
                for (const Json::Value &attribute_json : send_meta_data_json["receive"][object_name])
                {
                    const std::string attribute_name = attribute_json.asString();
                    if (!attribute_name.empty())
                    {
                        continue;
                    }

                    receive_objects_json[object_name] = {};
                    for (const std::pair<std::string, std::pair<std::vector<double>, bool>> &attribute : worlds[world_name][object_name])
                    {
                        if ((strcmp(attribute.first.c_str(), "force") != 0 && strcmp(attribute.first.c_str(), "torque") != 0 ||
                             attribute.second.first.size() > 3) &&
                            std::find(receive_objects_json[object_name].begin(), receive_objects_json[object_name].end(), attribute.first) == receive_objects_json[object_name].end())
                        {
                            receive_objects_json[object_name].append(attribute.first);
                        }
                    }
                    break;
                }
            }
            else
            {
                for (const Json::Value &attribute_json : send_meta_data_json["receive"][object_name])
                {
                    const std::string attribute_name = attribute_json.asString();
                    for (const std::pair<std::string, std::map<std::string, std::pair<std::vector<double>, bool>>> &object : worlds[world_name])
                    {
                        if (object.second.count(attribute_name) != 0 &&
                            (strcmp(attribute_name.c_str(), "force") != 0 && strcmp(attribute_name.c_str(), "torque") != 0 ||
                             object.second.at(attribute_name).first.size() > 3) &&
                            std::find(receive_objects_json[object.first].begin(), receive_objects_json[object.first].end(), attribute_name) == receive_objects_json[object.first].end())
                        {
                            receive_objects_json[object.first].append(attribute_name);
                        }
                    }
                }
                receive_objects_json.removeMember(object_name);
                break;
            }
        }
    }

    void wait_for_objects()
    {
        int start = get_time_now();
        int now = get_time_now();
        bool found_all_objects = true;
        do
        {
            found_all_objects = true;
            now = get_time_now();
            for (const std::string &object_name : receive_objects_json.getMemberNames())
            {
                for (const Json::Value &attribute_json : receive_objects_json[object_name])
                {
                    const std::string attribute_name = attribute_json.asString();
                    if ((worlds[world_name].count(object_name) == 0 || worlds[world_name][object_name].count(attribute_name) == 0))
                    {
                        found_all_objects = false;
                        if (now - start > 1)
                        {
                            ROS_INFO("[Server] Socket [%s] is waiting for [%s][%s] to be declared.", socket_addr.c_str(), object_name.c_str(), attribute_name.c_str());
                        }
                    }
                }
            }
            if (now - start > 1)
            {
                start = now;
            }
        } while (!should_shut_down && !found_all_objects);
    }

    void bind_receive_objects()
    {
        for (const std::string &object_name : receive_objects_json.getMemberNames())
        {
            for (const Json::Value &attribute_json : receive_objects_json[object_name])
            {
                const std::string attribute_name = attribute_json.asString();

                const size_t data_size = (strcmp(attribute_name.c_str(), "force") == 0 || strcmp(attribute_name.c_str(), "torque") == 0) ? 3 : worlds[world_name][object_name][attribute_name].first.size();
                for (size_t i = 0; i < data_size; i++)
                {
                    double *data = &worlds[world_name][object_name][attribute_name].first[i];
                    const double conversion = 1.0 / conversion_map[attribute_map[attribute_name].first][i];
                    receive_data_vec.emplace_back(data, conversion);
                    receive_meta_data_json["receive"][object_name][attribute_name].append(*data * conversion);
                }
            }
        }
    }

    void response_meta_data()
    {
        send_buffer_size = 1 + send_data_vec.size();
        receive_buffer_size = 1 + receive_data_vec.size();

        if (should_shut_down)
        {
            receive_meta_data_json["time"] = -1.0;
        }

        if (continue_state)
        {
            continue_state = false;
        }

        const std::string message_str = receive_meta_data_json.toStyledString();

        // Send buffer sizes and send_data (if exists) over ZMQ
        zmq::message_t response_message(message_str.size());
        memcpy(response_message.data(), message_str.data(), message_str.size());
        socket_server.send(response_message, zmq::send_flags::none);

        send_buffer = (double *)calloc(send_buffer_size, sizeof(double));
        receive_buffer = (double *)calloc(receive_buffer_size, sizeof(double));
    }

    void request_send_data()
    {
        // Receive send_data over ZMQ
        try
        {
            sockets_need_clean_up[socket_addr] = false;
            socket_server.recv(message, zmq::recv_flags::none);
            sockets_need_clean_up[socket_addr] = true;
            memcpy(send_buffer, message.data(), send_buffer_size * sizeof(double));
        }
        catch (const zmq::error_t &e)
        {
            should_shut_down = true;
            ROS_INFO("[Server] %s, socket at [%s] prepares to close.", e.what(), socket_addr.c_str());
        }
    }

    void wait_for_receive_data()
    {
        if (!is_receive_data_sent)
        {
            for (const std::string &object_name : send_objects_json.getMemberNames())
            {
                for (const Json::Value &attribute_json : send_objects_json[object_name])
                {
                    const std::string attribute_name = attribute_json.asString();
                    worlds[world_name][object_name][attribute_name].second = true;
                }
            }

            for (const std::string object_name : receive_objects_json.getMemberNames())
            {
                for (const Json::Value &attribute_json : receive_objects_json[object_name])
                {
                    const std::string attribute_name = attribute_json.asString();
                    int start = get_time_now();
                    while ((worlds[world_name].count(object_name) == 0 || worlds[world_name][object_name].count(attribute_name) == 0 || !worlds[world_name][object_name][attribute_name].second) && !should_shut_down)
                    {
                        const int now = get_time_now();
                        if (now - start > 1)
                        {
                            ROS_INFO("[Server] Socket at [%s] is waiting for data of [%s][%s] to be sent.", socket_addr.c_str(), object_name.c_str(), attribute_name.c_str());
                            start = now;
                        }
                    }
                }
            }

            is_receive_data_sent = true;
        }
    }

    void compute_cumulative_data()
    {
        for (const std::string &object_name : receive_objects_json.getMemberNames())
        {
            for (const std::string &effort : {"force", "torque"})
            {
                if (std::find(receive_objects_json.begin(), receive_objects_json.end(), effort) == receive_objects_json.end())
                {
                    continue;
                }

                for (std::pair<const std::string, std::map<std::string, std::vector<double>>> &send_effort : send_efforts[world_name][object_name])
                {
                    for (size_t i = 0; i < 3; i++)
                    {
                        for (size_t j = 3; j < send_effort.second[effort].size() / 3; j += 3)
                        {
                            send_effort.second[effort][i] += send_effort.second[effort][j];
                        }

                        worlds[world_name][object_name][effort].first[i] = send_effort.second[effort][i];
                    }
                }
            }
        }
    }

    void response_receive_data()
    {
        // Send receive_data over ZMQ
        receive_buffer[0] = should_shut_down ? -1.0 : get_time_now();
        zmq::message_t reply_data(receive_buffer_size * sizeof(double));
        memcpy(reply_data.data(), receive_buffer, receive_buffer_size * sizeof(double));
        socket_server.send(reply_data, zmq::send_flags::none);
    }

private:
    EFlag flag = EFlag::RequestSendMetaData;

    zmq::message_t message;

    std::string message_str;

    std::string socket_addr;

    zmq::socket_t socket_server;

    Json::Value send_meta_data_json;

    Json::Value send_objects_json;

    Json::Value receive_meta_data_json;

    Json::Value receive_objects_json;

    size_t send_buffer_size = 1;

    size_t receive_buffer_size = 1;

    double *send_buffer;

    double *receive_buffer;

    std::vector<std::pair<std::vector<double>::iterator, double>> send_data_vec;

    std::vector<std::pair<std::vector<double>::iterator, double>> receive_data_vec;

    std::map<EAttribute, std::vector<double>> conversion_map;

    std::string world_name;

    Json::Reader reader;

    bool is_receive_data_sent;

    bool continue_state = false;
};

void start_multiverse_server(int port)
{
    MultiverseServer multiverse_server("tcp://127.0.0.1:" + std::to_string(port));
    multiverse_server.start();
}

int main(int argc, char **argv)
{
    // register signal SIGINT and signal handler
    signal(SIGINT, [](int signum)
           {
        ROS_INFO("[Server] Interrupt signal (%d) received, wait for 1s then shutdown.", signum);
        should_shut_down = true; });

    ros::init(argc, argv, "state_server");

    for (size_t thread_num = 0; thread_num < argc - 1; thread_num++)
    {
        workers.emplace_back(start_multiverse_server, std::stoi(argv[thread_num + 1]));
    }

    while (!should_shut_down)
    {
    }

    bool can_shut_down = true;
    do
    {
        can_shut_down = true;
        for (const std::pair<std::string, bool> &socket_needs_clean_up : sockets_need_clean_up)
        {
            if (socket_needs_clean_up.second)
            {
                can_shut_down = false;
                break;
            }
        }
    } while (!can_shut_down);

    zmq_sleep(1);

    context.shutdown();

    for (std::thread &worker : workers)
    {
        worker.join();
    }
}
