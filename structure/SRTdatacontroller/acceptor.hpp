#pragma once
#include <memory>
#include <vector>
#include <future>
#include "srt_socket.hpp"

std::vector<std::future<void>> accepting_threads;

void async_accept(std::shared_ptr<xtransmit::socket::srt> s)
{
    auto accept_future = std::async(std::launch::async, [s = std::move(s)]() mutable {
        try {
            while (true) {
                auto s_accepted = s->async_accept();
            }
        } catch (...) {
        }
    });

    accepting_threads.push_back(std::move(accept_future));
}