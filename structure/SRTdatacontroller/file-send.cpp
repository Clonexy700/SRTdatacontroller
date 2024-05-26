#include <iostream>
#include <iterator>
#include <filesystem>	// Requires C++17
#include <functional>
#include <string>
#include <vector>
#include <deque>
#include <chrono>
#include <thread>

#include "file-send.hpp"
#include "srt_socket.hpp"

using namespace std;
using namespace std::chrono;
using namespace srtdatacontroller;
using namespace srtdatacontroller::file::send;
namespace fs = std::filesystem;

using shared_srt = std::shared_ptr<socket::srt>;

#if ENABLE_FILE_TRANSFER

bool sendFile(const string& filename, const string& uploadName, socket::srt& dst,
    vector<char>& buf, const atomic_bool& forceBreak) {
    ifstream ifile(filename, ios::binary);
    if (!ifile) {
        cerr << "Error opening file: " << filename << endl;
        return false;
    }

    const chrono::steady_clock::time_point timeStart = chrono::steady_clock::now();
    size_t fileSize = 0;

    cerr << "Transmitting '" << filename << "' to " << uploadName << endl;

    int hdrSize = snprintf(buf.data() + 1, buf.size(), "%s", uploadName.c_str()) + 2;

    while (!forceBreak) {
        const int n = static_cast<int>(ifile.read(buf.data() + hdrSize, static_cast<streamsize>(buf.size() - hdrSize)).gcount());
        const bool isEof = ifile.eof();
        const bool isStart = hdrSize > 1;
        buf[0] = (isEof ? 2 : 0) | (isStart ? 1 : 0);

        size_t shift = 0;
        while (n > 0) {
            const int st = dst.write(const_buffer(buf.data() + shift, n + hdrSize));
            if (st == 0) continue;

            fileSize += n;

            if (st == SRT_ERROR) {
                cerr << "Upload: SRT error: " << srt_getlasterror_str() << endl;
                return false;
            }
            if (st != n + hdrSize) {
                cerr << "Upload error: not fully delivered" << endl;
                return false;
            }

            shift += st - hdrSize;
            hdrSize = 1;
            break;
        }

        if (isEof) break;

        if (!ifile.good()) {
            cerr << "ERROR while reading from file\n";
            return false;
        }
    }

    const chrono::steady_clock::time_point timeEnd = chrono::steady_clock::now();
    const auto deltaUs = chrono::duration_cast<chrono::microseconds>(timeEnd - timeStart).count();

    const size_t rateKbps = (fileSize * 1000) / deltaUs * 8;
    cerr << "--> done (" << fileSize / 1024 << " kbytes transferred at " << rateKbps << " kbps, took "
        << chrono::duration_cast<chrono::seconds>(timeEnd - timeStart).count() << " s" << endl;

    return true;
}

const vector<string> readDirectory(const string& path) {
    vector<string> filenames;
    deque<string> subdirs = { path };

    while (!subdirs.empty()) {
        fs::path p(subdirs.front());
        subdirs.pop_front();

        if (!fs::is_directory(p)) {
            filenames.push_back(p.string());
            continue;
        }

        for (const auto& entry : fs::directory_iterator(p)) {
            if (entry.is_directory())
                subdirs.push_back(entry.path().string());
            else
                filenames.push_back(entry.path().string());
        }
    }

    return filenames;
}

const string relativePath(const string& filepath, const string& dirpath) {
    const fs::path dir(dirpath);
    const fs::path file(filepath);
    if (dir == file) return file.filename().string();

    const size_t pos = file.string().find(dir.string());
    if (pos != 0) {
        cerr << "Failed to find substring" << endl;
        return string();
    }

    return file.generic_string().erase(pos, dir.generic_string().size());
}

void startFileSender(future<shared_srt> connection, const config& cfg,
    const vector<string>& filenames, const atomic_bool& forceBreak) {
    if (!connection.valid()) {
        cerr << "Error: Unexpected socket creation failure!" << endl;
        return;
    }

    const shared_srt sock = connection.get();
    if (!sock) {
        cerr << "Error: Unexpected socket connection failure!" << endl;
        return;
    }

    atomic_bool localBreak(false);

    auto statsFunc = [&cfg, &forceBreak, &localBreak](shared_srt sock) {
        if (cfg.statsFreqMs == 0) return;
        if (cfg.statsFile.empty()) return;

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

    socket::srt dstSock = *sock.get();

    vector<char> buf(cfg.segmentSize);
    for (const string& fname : filenames) {
        const bool transmitRes = sendFile(fname, relativePath(fname, cfg.srcPath),
            dstSock, buf, forceBreak);

        if (!transmitRes) break;

        if (forceBreak) break;
    }

    size_t blocks = 0;
    do {
        if (SRT_ERROR == srt_getsndbuffer(dstSock.id(), &blocks, nullptr)) break;
        if (blocks) this_thread::sleep_for(chrono::milliseconds(5));
    } while (blocks != 0);

    localBreak = true;
    statsLogger.wait();
}

void xtransmit::file::send::run(const string& dstUrl, const config& cfg, const atomic_bool& forceBreak) {
    const vector<string> filenames = readDirectory(cfg.srcPath);

    if (filenames.empty()) {
        cerr << "Found no files to transmit (path " << cfg.srcPath << ")" << endl;
        return;
    }

    if (cfg.onlyPrint) {
        cout << "Files found in " << cfg.srcPath << endl;

        for_each(filenames.begin(), filenames.end(),
            [&dirpath = std::as_const(cfg.srcPath)](const string& fname) {
                cout << fname << endl;
                cout << "RELATIVE: " << relativePath(fname, dirpath) << endl;
            });
        return;
    }

    UriParser ut(dstUrl);
    ut["transtype"] = string("file");
    ut["messageapi"] = string("true");
    ut["blocking"] = string("true");
    if (!ut["sndbuf"].exists()) ut["sndbuf"] = to_string(cfg.segmentSize * 10);

    shared_srt socket = make_shared<socket::srt>(ut);
    const bool accept = socket->mode() == socket::srt::LISTENER;
    try {
        startFileSender(accept ? socket->asyncAccept() : socket->asyncConnect(),
            cfg, filenames, forceBreak);
    } catch (const socket::exception& e) {
        cerr << e.what() << endl;
        return;
    }
}

CLI::App* xtransmit::file::send::addSubcommand(CLI::App& app, config& cfg, string& dstUrl) {
    const map<string, int> toMs{ {"s", 1'000}, {"ms", 1} };

    CLI::App* scFileSend = app.add_subcommand("send", "Send file or folder")->fallthrough();
    scFileSend->add_option("src", cfg.srcPath, "Source path to file/folder");
    scFileSend->add_option("dst", dstUrl, "Destination URI");
    scFileSend->add_flag("--printout", cfg.onlyPrint, "Print files found in a folder ad subfolders. No transfer.");
    scFileSend->add_option("--segment", cfg.segmentSize, "Size of the transmission segment");
    scFileSend->add_option("--statsfile", cfg.statsFile, "output stats report filename");
    scFileSend->add_option("--statsformat", cfg.statsFormat, "output stats report format (json, csv)");
    scFileSend->add_option("--statsfreq", cfg.statsFreqMs, "output stats report frequency (ms)")
        ->transform(CLI::AsNumberWithUnit(toMs, CLI::AsNumberWithUnit::CASE_SENSITIVE));

    return scFileSend;
}