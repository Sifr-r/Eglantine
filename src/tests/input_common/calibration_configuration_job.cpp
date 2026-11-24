// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <boost/asio.hpp>
#include <boost/crc.hpp>
#include <catch2/catch_test_macros.hpp>

#include "input_common/drivers/udp_client.h"
#include "input_common/helpers/udp_protocol.h"


class FakeCemuhookServer {
public:
    FakeCemuhookServer()
        : socket(io_context,
                 boost::asio::ip::udp::endpoint(boost::asio::ip::address_v4::loopback(), 0)) {}

    ~FakeCemuhookServer() {
        io_context.stop();
        if (handler.joinable()) {
            handler.join();
        }
        boost::system::error_code error_code;
        socket.shutdown(boost::asio::socket_base::shutdown_both, error_code);
        socket.close(error_code);
    }

    u16 GetPort() {
        return socket.local_endpoint().port();
    }

    std::string GetHost() {
        return socket.local_endpoint().address().to_string();
    }

    void Run(std::vector<InputCommon::CemuhookUDP::Response::TouchPad> touch_movement_path) {
        REQUIRE(touch_movement_path.size() > 0);
        touch_path = std::move(touch_movement_path);
        touch_index = 0;
        StartReceive();
        handler = std::jthread([this] { io_context.run(); });
    }

private:
    void StartReceive() {
        socket.async_receive_from(
            boost::asio::buffer(receive_buffer), sender_endpoint,
            [this](const boost::system::error_code& ec, std::size_t received_size) {
                if (ec) {
                    return;
                }

                constexpr size_t HeaderSize = sizeof(InputCommon::CemuhookUDP::Header);
                constexpr size_t PadDataSize = sizeof(
                    InputCommon::CemuhookUDP::Message<InputCommon::CemuhookUDP::Response::PadData>);

                if (received_size >= HeaderSize) {
                    InputCommon::CemuhookUDP::Header header{};
                    std::memcpy(&header, receive_buffer.data(), HeaderSize);
                    switch (header.type) {
                    case InputCommon::CemuhookUDP::Type::PadData: {
                        InputCommon::CemuhookUDP::Response::PadData pad_data{};
                        pad_data.touch[0] = touch_path[touch_index];
                        const auto pad_message = InputCommon::CemuhookUDP::CreateMessage(
                            InputCommon::CemuhookUDP::SERVER_MAGIC, pad_data, 0);
                        std::memcpy(send_buffer.data(), &pad_message, PadDataSize);
                        boost::system::error_code send_ec;
                        socket.send_to(boost::asio::buffer(send_buffer, PadDataSize),
                                       sender_endpoint, 0, send_ec);

                        if (touch_index + 1 < touch_path.size()) {
                            ++touch_index;
                        }
                        break;
                    }
                    case InputCommon::CemuhookUDP::Type::PortInfo:
                    case InputCommon::CemuhookUDP::Type::Version:
                    default:
                        break;
                    }
                }

                StartReceive();
            });
    }

    boost::asio::io_context io_context;
    boost::asio::ip::udp::socket socket;
    boost::asio::ip::udp::endpoint sender_endpoint;
    std::array<u8, InputCommon::CemuhookUDP::MAX_PACKET_SIZE> send_buffer{};
    std::array<u8, InputCommon::CemuhookUDP::MAX_PACKET_SIZE> receive_buffer{};
    std::vector<InputCommon::CemuhookUDP::Response::TouchPad> touch_path;
    size_t touch_index{0};
    std::jthread handler;
};

TEST_CASE("CalibrationConfigurationJob completed", "[input_common]") {
    Common::Event complete_event;
    FakeCemuhookServer server;
    server.Run({{
                    .is_active = 1,
                    .x = 0,
                    .y = 0,
                },
                {
                    .is_active = 1,
                    .x = 200,
                    .y = 200,
                }});

    InputCommon::CemuhookUDP::CalibrationConfigurationJob::Status status{};
    u16 min_x{};
    u16 min_y{};
    u16 max_x{};
    u16 max_y{};
    bool completed{false};
    {
        InputCommon::CemuhookUDP::CalibrationConfigurationJob job(
            server.GetHost(), server.GetPort(),
            [&status,
             &complete_event](InputCommon::CemuhookUDP::CalibrationConfigurationJob::Status status_) {
                status = status_;
                if (status_ ==
                    InputCommon::CemuhookUDP::CalibrationConfigurationJob::Status::Completed) {
                    complete_event.Set();
                }
            },
            [&](u16 min_x_, u16 min_y_, u16 max_x_, u16 max_y_) {
                min_x = min_x_;
                min_y = min_y_;
                max_x = max_x_;
                max_y = max_y_;
            });

        completed = complete_event.WaitUntil(std::chrono::steady_clock::now() +
                                             std::chrono::seconds(10));
        job.Stop();
    }
    REQUIRE(completed);
    REQUIRE(status == InputCommon::CemuhookUDP::CalibrationConfigurationJob::Status::Completed);
    REQUIRE(min_x == 0);
    REQUIRE(min_y == 0);
    REQUIRE(max_x == 200);
    REQUIRE(max_y == 200);
}

TEST_CASE("CalibrationConfigurationJob cancellation joins its worker", "[input_common]") {
    FakeCemuhookServer server;
    for (unsigned i = 0; i < 32; ++i) {
        auto lifetime = std::make_shared<int>(0);
        std::weak_ptr<int> observer = lifetime;
        {
            InputCommon::CemuhookUDP::CalibrationConfigurationJob job(
                server.GetHost(), server.GetPort(),
                [lifetime](InputCommon::CemuhookUDP::CalibrationConfigurationJob::Status) {},
                [](u16, u16, u16, u16) {});
            lifetime.reset();
            job.Stop();
        }
        REQUIRE(observer.expired());
    }
}
