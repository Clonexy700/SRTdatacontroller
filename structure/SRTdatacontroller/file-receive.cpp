#include <iostream>
#include <fstream>
#include <filesystem>    // Requires C++17
#include <functional>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <future>

#include "file-receive.hpp"
#include "srt_socket.hpp"

namespace fs = std::filesystem;

using namespace std;
using namespace std::chrono;
using namespace srtdatacontroller;
using namespace srtdatacontroller::file::receive;

using SharedSrt = std::shared_ptr<socket::srt>;

bool createFolder(const string& path) {
    error_code ec;
    if (fs::create_directory(path, ec)) {
        cerr << "Created directory '" << path << "'" << endl;
        return true;
    }

    if (ec) {
        cerr << "Failed to create the directory '" << path << "'. Error " << ec.message() << endl;
        return false;
    }
    return true;
}

bool createSubfolders(const string& path) {
    size_t found = path.find("./");
    if (found == std::string::npos) {
        found = path.find(".\\");
    }

    size_t pos = found != std::string::npos ? (found + 2) : 0;
    const size_t lastDelim = path.find_last_of("/\\");
    if (lastDelim == string::npos || lastDelim < pos) {
        cerr << "No folders to create\n";
        return true;
    }

    while (pos != std::string::npos && pos != lastDelim) {
        pos = path.find_first_of("\\/", pos + 1);
        if (!createFolder(path.substr(0, pos).c_str())) {
            return false;
        }
    }

    return true;
}

bool receiveFiles(socket::srt& src, const string& dstPath, vector<char>& buf, const atomic_bool& forceBreak) {
    cerr << "Downloading to '" << dstPath << endl;

    steady_clock::time_point timeStart;
    steady_clock::time_point timeProgress;
    size_t fileSize = 0;

    string downloadStr = "";

    ofstream ofile;
    while (!forceBreak) {
        const size_t bytes = src.read(mutable_buffer(buf.data(), buf.size()), -1);
        if (bytes == 0) {
            continue;
        }

        int hdrSize = 1;
        const bool isFirst = (buf[0] & 0x01) != 0;
        const bool isEof = (buf[0] & 0x02) != 0;
        const auto tNow = steady_clock::now();

        if (isFirst) {
            ofile.close();
            const string filename = string(buf.data() + 1);
            const string filepath = dstPath + filename;
            hdrSize += filename.size() + 1;

            if (!createSubfolders(filepath)) {
                cerr << "Download: failed creating folders for '" << filepath << "'" << endl;
                return false;
            }

            ofile.open(filepath.c_str(), ios::out | ios::trunc | ios::binary);
            if (!ofile) {
                cerr << "Download: error opening file " << filepath << endl;
                break;
            }

            downloadStr = "Downloading '" + filename + "'";
            cerr << downloadStr << "\r";
            timeStart = timeProgress = tNow;
            fileSize = 0;
        }

        if (!ofile) {
            cerr << "Download: file is closed while data is received: first packet missed?\n";
            continue;
        }

        ofile.write(buf.data() + hdrSize, bytes - hdrSize);
        fileSize += bytes - hdrSize;

        auto getRateKbps = [](steady_clock::time_point tStart, steady_clock::time_point tNow, size_t bytes) {
            const auto deltaUs = duration_cast<microseconds>(tNow - tStart).count();
            const size_t rateKbps = (bytes * 1000) / (deltaUs ? deltaUs : 1) * 8;
            return rateKbps;
        };

        if (steady_clock::now() >= timeProgress + 1s) {
            const size_t rateKbps = getRateKbps(timeStart, tNow, fileSize);
            cerr << downloadStr << ": " << fileSize / 1024 << " kB @ " << rateKbps << " kbps...\r";
            timeProgress = tNow;
        }

        if (isEof) {
            ofile.close();
            const size_t rateKbps = getRateKbps(timeStart, tNow, fileSize);
            const auto deltaMs = duration_cast<milliseconds>(tNow - timeStart).count();
            cerr << downloadStr << ": done (" << fileSize / 1024 << " kB @ " << rateKbps << " kbps, took "
                 << deltaMs / 1000.0 << " sec)." << endl;
        }
    }

    return true;
}

void startFileReceiver(future<SharedSrt> connection, const config& cfg, const atomic_bool& forceBreak) {
    if (!connection.valid()) {
        cerr << "Error: Unexpected socket creation failure!" << endl;
        return;
    }

    const SharedSrt sock = connection.get();
    if (!sock) {
        cerr << "Error: Unexpected socket connection failure!" << endl;
        return;
    }

    atomic_bool localBreak(false);

    auto statsFunc = [&cfg, &forceBreak, &localBreak](SharedSrt sock) {
        if (cfg.statsFreqMs == 0) {
            return;
        }
        if (cfg.statsFile.empty()) {
            return;
        }

        ofstream logfileStats(cfg.statsFile.c_str());
        if (!logfileStats) {
            cerr << "ERROR: Can't open '" << cfg.statsFile << "' for writing stats. No output.\n";
            return;
        }

        bool printHeader = true;
        const milliseconds interval(cfg.statsFreqMs);
        while (!forceBreak && !localBreak) {
            this_thread::sleep_for(interval);

            logfileStats << sock->getStatistics(cfg.statsFormat, printHeader) << flush;
            printHeader = false;
        }
    };
    auto statsLogger = async(launch::async, statsFunc, sock);

    vector<char> buf(cfg.segmentSize);
    receiveFiles(*sock.get(), cfg.dstPath, buf, forceBreak);

    localBreak = true;
    statsLogger.wait();
	}

void srtdatacontroller::file::receive::run(const string& srcUrl, const config& cfg, const atomic_bool& forceBreak) {
    UriParser ut(srcUrl);
    ut["transtype"] = string("file");
    ut["messageapi"] = string("true");
    if (!ut["rcvbuf"].exists()) {
        ut["rcvbuf"] = to_string(cfg.segmentSize * 10);
    }

    SharedSrt socket = make_shared<socket::srt>(ut);
    const bool accept = socket->mode() == socket::srt::LISTENER;
    try {
        startFileReceiver(accept ? socket->asyncAccept() : socket->asyncConnect(), cfg, forceBreak);
    } catch (const socket::exception& e) {
        cerr << e.what() << endl;
        return;
    }
}

CLI::App* srtdatacontroller::file::receive::addSubcommand(CLI::App& app, config& cfg, string& srcUrl) {
    const map<string, int> toMs{{"s", 1'000}, {"ms", 1}};

    CLI::App* scFileRecv = app.add_subcommand("receive", "Receive file or folder")->fallthrough();
    scFileRecv->add_option("src", srcUrl, "Source URI");
    scFileRecv->add_option("dst", cfg.dstPath, "Destination path to file/folder");
    scFileRecv->add_option("--segment", cfg.segmentSize, "Size of the transmission segment");
    scFileRecv->add_option("--statsfile", cfg.statsFile, "output stats report filename");
    scFileRecv->add_option("--statsformat", cfg.statsFormat, "output stats report format (json, csv)");
    scFileRecv->add_option("--statsfreq", cfg.statsFreqMs, "output stats report frequency (ms)")
        ->transform(CLI::AsNumberWithUnit(toMs, CLI::AsNumberWithUnit::CASE_SENSITIVE));

    return scFileRecv;
}