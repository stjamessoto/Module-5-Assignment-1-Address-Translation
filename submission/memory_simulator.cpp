// =============================================================================
//  memory_simulator.cpp
//  CS375 A9 - Advanced Segmented, Paged Memory Model Simulator
//
//  A segmented, two-level paged virtual memory simulator with:
//    * segment + page protection (read-only / read-write)
//    * demand paging with FIFO or LRU page replacement
//    * a two-level page table (page directory -> page table) per segment
//    * runtime / file-based memory configuration with validation
//    * stress testing, batch processing, error logging, latency tracking
//    * a text memory map, a Gantt-like timeline and policy / page-size studies
//
//  Build:  g++ -std=c++17 -Wall -Wextra -O2 -o memory_simulator memory_simulator.cpp
//  Run:    ./memory_simulator [--seed N] [--config FILE] [--color | --no-color]
//
//  Logical address = (segment, page directory index, page number, offset, access)
//  Physical address = frame * page_size + offset
// =============================================================================
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#define ISATTY _isatty
#define FILENO _fileno
#else
#include <unistd.h>
#define ISATTY isatty
#define FILENO fileno
#endif

// ----------------------------------------------------------------------------
//  Constants and small helpers
// ----------------------------------------------------------------------------
const int PAGE_FAULT_PENALTY = 10;      // extra latency units to service a page fault
const int PTE_SIZE_BYTES = 4;           // size of one directory / page-table entry
const int MAX_FRAMES = 4096;
const int MAX_PAGE_SIZE = 1000000;
const int MAX_SEGMENTS = 32;
const int MAX_PAGES_PER_SEGMENT = 4096;
const int MAX_PAGES_PER_DIR = 1024;

const char* RESULTS_FILE = "results.txt";
const char* BATCH_RESULTS_FILE = "batch_results.txt";
const char* ERROR_LOG_FILE = "errors.log";
const char* LATENCY_HISTORY_FILE = "latency_history.csv";

enum Protection { READ_ONLY, READ_WRITE };
enum AccessType { READ, WRITE };
enum Policy { FIFO, LRU };

const char* protName(Protection p) { return p == READ_ONLY ? "RO" : "RW"; }
const char* accessName(AccessType a) { return a == READ ? "READ" : "WRITE"; }
const char* policyName(Policy p) { return p == FIFO ? "FIFO" : "LRU"; }

std::string S(long v) { return std::to_string(v); }

std::string hexStr(long v) {
    std::ostringstream o;
    o << "0x" << std::hex << std::uppercase << v;
    return o.str();
}

std::string fixed2(double v, int digits = 2) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(digits) << v;
    return o.str();
}

std::string pct(double part, double whole) {
    return whole <= 0 ? "0.0%" : fixed2(100.0 * part / whole, 1) + "%";
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string upper(std::string s) {
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

// Splits on whitespace, commas and parentheses so "(1, 0, 2, 250, R)" works too.
std::vector<std::string> tokenize(std::string s) {
    for (char& c : s)
        if (c == ',' || c == '(' || c == ')' || c == '\t') c = ' ';
    std::istringstream in(s);
    std::vector<std::string> out;
    std::string t;
    while (in >> t) out.push_back(t);
    return out;
}

bool parseLong(const std::string& s, long& out, int base = 10) {
    if (s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    long v = std::strtol(s.c_str(), &end, base);
    if (*end != '\0' || errno != 0) return false;
    out = v;
    return true;
}

bool parseProtection(const std::string& s, Protection& out) {
    std::string u = upper(s);
    if (u == "RO" || u == "R" || u == "READ_ONLY" || u == "READONLY") { out = READ_ONLY; return true; }
    if (u == "RW" || u == "W" || u == "READ_WRITE" || u == "READWRITE") { out = READ_WRITE; return true; }
    return false;
}

bool parseAccess(const std::string& s, AccessType& out) {
    std::string u = upper(s);
    if (u == "R" || u == "READ" || u == "0") { out = READ; return true; }
    if (u == "W" || u == "WRITE" || u == "1") { out = WRITE; return true; }
    return false;
}

bool parsePolicy(const std::string& s, Policy& out) {
    std::string u = upper(s);
    if (u == "FIFO" || u == "F" || u == "1") { out = FIFO; return true; }
    if (u == "LRU" || u == "L" || u == "2") { out = LRU; return true; }
    return false;
}

std::string nowString() {
    std::time_t t = std::time(nullptr);
    char buf[64];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    return buf;
}

// ----------------------------------------------------------------------------
//  Terminal UI helpers (colors, prompts, input echo when input is piped)
// ----------------------------------------------------------------------------
namespace ui {
bool color = false;
bool echoInput = false;

std::string paint(const char* code, const std::string& s) {
    return color ? std::string("\033[") + code + "m" + s + "\033[0m" : s;
}
std::string good(const std::string& s) { return paint("32", s); }
std::string warn(const std::string& s) { return paint("33", s); }
std::string bad(const std::string& s) { return paint("31", s); }
std::string head(const std::string& s) { return paint("1;36", s); }
std::string bold(const std::string& s) { return paint("1", s); }
std::string dim(const std::string& s) { return paint("2", s); }

void banner(const std::string& title) {
    std::string line(64, '=');
    std::cout << "\n" << head(line) << "\n" << head("  " + title) << "\n" << head(line) << "\n";
}
}  // namespace ui

struct InputEnded {};

std::string readLine(const std::string& prompt) {
    std::cout << prompt << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) {
        std::cout << "\n";
        throw InputEnded{};
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (ui::echoInput) std::cout << line << "\n";   // makes piped runs look like typed sessions
    return trim(line);
}

long readInt(const std::string& prompt, long lo, long hi, long def) {
    while (true) {
        std::string line = readLine(prompt + " [" + S(def) + "]: ");
        if (line.empty()) return def;
        long v;
        if (parseLong(line, v) && v >= lo && v <= hi) return v;
        std::cout << ui::bad("  Please enter a whole number from " + S(lo) + " to " + S(hi) + ".") << "\n";
    }
}

double readDouble(const std::string& prompt, double lo, double hi, double def) {
    while (true) {
        std::string line = readLine(prompt + " [" + fixed2(def) + "]: ");
        if (line.empty()) return def;
        char* end = nullptr;
        double v = std::strtod(line.c_str(), &end);
        if (*end == '\0' && v >= lo && v <= hi) return v;
        std::cout << ui::bad("  Please enter a number from " + fixed2(lo) + " to " + fixed2(hi) + ".") << "\n";
    }
}

bool readYesNo(const std::string& prompt, bool def) {
    while (true) {
        std::string line = upper(readLine(prompt + (def ? " (y/n) [y]: " : " (y/n) [n]: ")));
        if (line.empty()) return def;
        if (line == "Y" || line == "YES") return true;
        if (line == "N" || line == "NO") return false;
        std::cout << ui::bad("  Please answer y or n.") << "\n";
    }
}

// ----------------------------------------------------------------------------
//  Core data structures
// ----------------------------------------------------------------------------
struct Page {
    int frame_number = -1;   // -1 if not in memory
    bool present = false;    // presence bit
    Protection protection = READ_WRITE;
    int last_access = 0;     // reference time, used by LRU
};

struct Segment {
    int base_address;        // physical address of this segment's page directory
    int limit;               // number of pages in the segment
    Protection protection;
};

enum class Lookup { HIT, NOT_PRESENT, INVALID_PAGE, PROTECTION };

// Second-level table: maps page numbers to frames.
class PageTable {
public:
    std::vector<Page> pages;
    int page_size;

    PageTable(int numPages, int pageSize, Protection prot) : pages(numPages), page_size(pageSize) {
        for (auto& p : pages) p.protection = prot;
    }

    // Returns the frame on a hit, -1 otherwise; `result` says why.
    int getFrameNumber(int pageNum, int time, AccessType access, Lookup& result) {
        if (pageNum < 0 || pageNum >= (int)pages.size()) {
            result = Lookup::INVALID_PAGE;
            return -1;
        }
        Page& p = pages[pageNum];
        if (access == WRITE && p.protection == READ_ONLY) {
            result = Lookup::PROTECTION;
            return -1;
        }
        if (!p.present) {
            result = Lookup::NOT_PRESENT;
            return -1;
        }
        p.last_access = time;
        result = Lookup::HIT;
        return p.frame_number;
    }

    // Maps a page to a frame. The page keeps its own protection bits.
    void setFrame(int pageNum, int frame, int time) {
        if (pageNum >= 0 && pageNum < (int)pages.size()) {
            pages[pageNum].frame_number = frame;
            pages[pageNum].present = true;
            pages[pageNum].last_access = time;
        }
    }

    // Victim side of page replacement: evicts the page and returns the frame it held.
    int replacePage(int pageNum) {
        if (pageNum < 0 || pageNum >= (int)pages.size()) return -1;
        int frame = pages[pageNum].frame_number;
        pages[pageNum].frame_number = -1;
        pages[pageNum].present = false;
        return frame;
    }

    int presentCount() const {
        return (int)std::count_if(pages.begin(), pages.end(), [](const Page& p) { return p.present; });
    }
};

// First-level table (page directory): maps directory indices to page tables.
// Entries are allocated lazily, so unused parts of a segment cost no page-table memory.
class DirectoryTable {
public:
    int total_pages;
    int pages_per_dir;
    int page_size;
    Protection default_prot;
    std::vector<std::optional<PageTable>> entries;

    DirectoryTable(int totalPages, int pagesPerDir, int pageSize, Protection prot)
        : total_pages(totalPages), pages_per_dir(pagesPerDir), page_size(pageSize), default_prot(prot),
          entries((totalPages + pagesPerDir - 1) / pagesPerDir) {}

    int numDirs() const { return (int)entries.size(); }
    int pagesInDir(int d) const { return std::min(pages_per_dir, total_pages - d * pages_per_dir); }
    bool isAllocated(int d) const { return entries[d].has_value(); }
    PageTable* get(int d) { return entries[d] ? &*entries[d] : nullptr; }

    PageTable& allocate(int d) {
        if (!entries[d]) entries[d].emplace(pagesInDir(d), page_size, default_prot);
        return *entries[d];
    }

    int allocatedTables() const {
        return (int)std::count_if(entries.begin(), entries.end(), [](const auto& e) { return e.has_value(); });
    }
    int allocatedEntries() const {
        int n = 0;
        for (const auto& e : entries)
            if (e) n += (int)e->pages.size();
        return n;
    }
};

struct FrameOwner {
    int seg = -1, dir = -1, page = -1;
};

std::string ownerName(const FrameOwner& o) {
    return "S" + S(o.seg) + ":D" + S(o.dir) + ":P" + S(o.page);
}

// Fixed-size pool of physical frames plus the bookkeeping FIFO and LRU need.
class PhysicalMemory {
public:
    int num_frames;
    Policy policy;
    std::vector<bool> free_frames;
    std::vector<FrameOwner> owner;
    std::vector<int> last_used;     // LRU: last reference time per frame
    std::deque<int> fifo_queue;     // FIFO: frames in load order (front = oldest)
    int time = 0;                   // reference counter

    PhysicalMemory(int frames, Policy p)
        : num_frames(frames), policy(p), free_frames(frames, true), owner(frames), last_used(frames, 0) {}

    bool claimFrame(int frame, const FrameOwner& who) {
        if (frame < 0 || frame >= num_frames || !free_frames[frame]) return false;
        free_frames[frame] = false;
        owner[frame] = who;
        last_used[frame] = time;
        fifo_queue.push_back(frame);
        return true;
    }

    int allocateFrame(const FrameOwner& who) {
        for (int i = 0; i < num_frames; ++i)
            if (free_frames[i]) {
                claimFrame(i, who);
                return i;
            }
        return -1;   // memory full: caller must replace a page
    }

    int randomFreeFrame(std::mt19937& rng) const {
        std::vector<int> freeList;
        for (int i = 0; i < num_frames; ++i)
            if (free_frames[i]) freeList.push_back(i);
        return freeList.empty() ? -1 : freeList[rng() % freeList.size()];
    }

    int selectVictim() const {
        if (policy == FIFO) return fifo_queue.front();
        int victim = -1;
        for (int i = 0; i < num_frames; ++i)
            if (!free_frames[i] && (victim == -1 || last_used[i] < last_used[victim])) victim = i;
        return victim;
    }

    // Gives an occupied frame to a new page (after the old page was evicted).
    void reassign(int frame, const FrameOwner& who) {
        fifo_queue.erase(std::find(fifo_queue.begin(), fifo_queue.end(), frame));
        fifo_queue.push_back(frame);
        owner[frame] = who;
        last_used[frame] = time;
    }

    void touch(int frame) { last_used[frame] = time; }

    void freeFrame(int frame) {
        if (frame >= 0 && frame < num_frames && !free_frames[frame]) {
            free_frames[frame] = true;
            owner[frame] = FrameOwner{};
            auto it = std::find(fifo_queue.begin(), fifo_queue.end(), frame);
            if (it != fifo_queue.end()) fifo_queue.erase(it);
        }
    }

    int usedFrames() const { return (int)std::count(free_frames.begin(), free_frames.end(), false); }
    double utilization() const { return (double)usedFrames() / num_frames * 100; }
};

// ----------------------------------------------------------------------------
//  Metrics and translation results
// ----------------------------------------------------------------------------
enum class ErrorKind { NONE, SEGMENTATION_FAULT, DIRECTORY_FAULT, LIMIT_VIOLATION, OFFSET_FAULT, PROTECTION_VIOLATION };

const char* errorName(ErrorKind k) {
    switch (k) {
        case ErrorKind::SEGMENTATION_FAULT: return "SEGMENTATION FAULT";
        case ErrorKind::DIRECTORY_FAULT: return "DIRECTORY FAULT";
        case ErrorKind::LIMIT_VIOLATION: return "LIMIT VIOLATION";
        case ErrorKind::OFFSET_FAULT: return "OFFSET FAULT";
        case ErrorKind::PROTECTION_VIOLATION: return "PROTECTION VIOLATION";
        default: return "OK";
    }
}

struct Metrics {
    long requests = 0, successes = 0, hits = 0, page_faults = 0, replacements = 0, dir_allocations = 0;
    long seg_faults = 0, dir_faults = 0, limit_faults = 0, offset_faults = 0, prot_faults = 0;
    long total_latency = 0;

    long errors() const { return seg_faults + dir_faults + limit_faults + offset_faults + prot_faults; }
    double avgLatency() const { return requests ? (double)total_latency / requests : 0.0; }

    Metrics operator-(const Metrics& o) const {
        Metrics d;
        d.requests = requests - o.requests;
        d.successes = successes - o.successes;
        d.hits = hits - o.hits;
        d.page_faults = page_faults - o.page_faults;
        d.replacements = replacements - o.replacements;
        d.dir_allocations = dir_allocations - o.dir_allocations;
        d.seg_faults = seg_faults - o.seg_faults;
        d.dir_faults = dir_faults - o.dir_faults;
        d.limit_faults = limit_faults - o.limit_faults;
        d.offset_faults = offset_faults - o.offset_faults;
        d.prot_faults = prot_faults - o.prot_faults;
        d.total_latency = total_latency - o.total_latency;
        return d;
    }
};

struct TranslationResult {
    std::string logical;
    bool ok = false;
    long physical = -1;
    int frame = -1;
    bool page_fault = false;
    bool replaced = false;
    bool dir_allocated = false;
    FrameOwner evicted;
    int latency = 0;
    int start_time = 0;
    ErrorKind error = ErrorKind::NONE;
    std::string message;
    std::vector<std::string> trace;
};

struct TimelineEntry {
    long id;
    int start;
    int latency;
    std::string logical;
    std::string outcome;
    bool ok;
    bool page_fault;
};

std::string formatLogical(int s, int d, int p, int o, AccessType a) {
    return "(" + S(s) + "," + S(d) + "," + S(p) + "," + S(o) + "," + (a == READ ? "R" : "W") + ")";
}

std::string outcomeText(const TranslationResult& r) {
    if (!r.ok) return std::string(errorName(r.error)) + ": " + r.message;
    std::string s = "translated to " + S(r.physical);
    if (!r.page_fault) return s + "  [hit, frame " + S(r.frame) + "]";
    s += "  [PAGE FAULT -> frame " + S(r.frame);
    if (r.replaced) s += ", evicted " + ownerName(r.evicted);
    return s + "]";
}

// ----------------------------------------------------------------------------
//  Segment table: owns segments, their two-level page tables and physical memory
// ----------------------------------------------------------------------------
class SegmentTable {
public:
    std::vector<Segment> segments;
    std::map<int, DirectoryTable> pageTables;   // segment ID -> two-level page table
    PhysicalMemory physMem;
    int page_size;
    int pages_per_dir;
    Metrics metrics;
    std::vector<TimelineEntry> timeline;
    int clock = 0;                              // simulated time (sum of latencies)
    std::mt19937 rng;
    std::ostream* errorLog = nullptr;

    SegmentTable(int numFrames, int pageSize, int pagesPerDir, Policy policy, unsigned seed)
        : physMem(numFrames, policy), page_size(pageSize), pages_per_dir(pagesPerDir), rng(seed) {}

    // Sets the physical memory size. All pages become non-resident.
    void configureMemory(int numFrames, Policy policy) {
        physMem = PhysicalMemory(numFrames, policy);
        for (auto& [id, dt] : pageTables)
            for (auto& e : dt.entries)
                if (e)
                    for (auto& p : e->pages) {
                        p.present = false;
                        p.frame_number = -1;
                    }
    }

    bool addSegment(int id, int base, int limit, Protection prot, std::string& err) {
        if (id != (int)segments.size()) {
            err = "segment IDs must be 0,1,2,... in order (expected " + S(segments.size()) + ", got " + S(id) + ")";
            return false;
        }
        if ((int)segments.size() >= MAX_SEGMENTS) {
            err = "too many segments (max " + S(MAX_SEGMENTS) + ")";
            return false;
        }
        if (limit < 1 || limit > MAX_PAGES_PER_SEGMENT) {
            err = "segment " + S(id) + " limit " + S(limit) + " must be 1-" + S(MAX_PAGES_PER_SEGMENT) + " pages";
            return false;
        }
        if (base < 0) {
            err = "segment " + S(id) + " base address must not be negative";
            return false;
        }
        long dirBytes = (long)((limit + pages_per_dir - 1) / pages_per_dir) * PTE_SIZE_BYTES;
        for (size_t i = 0; i < segments.size(); ++i) {
            const Segment& o = segments[i];
            long oBytes = (long)((o.limit + pages_per_dir - 1) / pages_per_dir) * PTE_SIZE_BYTES;
            if (base < o.base_address + oBytes && o.base_address < base + dirBytes) {
                err = "page directory of segment " + S(id) + " at " + hexStr(base) + " overlaps segment " + S(i) +
                      " at " + hexStr(o.base_address);
                return false;
            }
        }
        segments.push_back({base, limit, prot});
        pageTables.emplace(id, DirectoryTable(limit, pages_per_dir, page_size, prot));
        return true;
    }

    // Makes virtual page `vpn` of `seg` resident in `frame`.
    bool loadPage(int seg, int vpn, int frame, std::string& err) {
        DirectoryTable& dt = pageTables.at(seg);
        int d = vpn / pages_per_dir, p = vpn % pages_per_dir;
        PageTable& pt = dt.allocate(d);
        if (frame < 0 || frame >= physMem.num_frames) {
            err = "frame " + S(frame) + " is out of range (0-" + S(physMem.num_frames - 1) + ")";
            return false;
        }
        if (!physMem.free_frames[frame]) {
            err = "frame " + S(frame) + " is already used by " + ownerName(physMem.owner[frame]);
            return false;
        }
        physMem.claimFrame(frame, {seg, d, p});
        pt.setFrame(p, frame, physMem.time);
        return true;
    }

    // Random segments and page tables; some directories empty, some pages not present.
    void initRandom(int numSegments) {
        for (int i = 0; i < numSegments; ++i) {
            int limit = 4 + rng() % 9;
            Protection prot = (rng() % 4 == 0) ? READ_ONLY : READ_WRITE;
            std::string err;
            addSegment(i, 0x1000 * (i + 1), limit, prot, err);
            DirectoryTable& dt = pageTables.at(i);
            for (int d = 0; d < dt.numDirs(); ++d) {
                if (d != 0 && rng() % 2 == 0) continue;   // leave this directory entry empty
                PageTable& pt = dt.allocate(d);
                for (int p = 0; p < (int)pt.pages.size(); ++p) {
                    if (prot == READ_WRITE && rng() % 5 == 0) pt.pages[p].protection = READ_ONLY;
                    if (rng() % 100 < 40) {
                        int f = physMem.randomFreeFrame(rng);
                        if (f >= 0) {
                            physMem.claimFrame(f, {i, d, p});
                            pt.setFrame(p, f, 0);
                        }
                    }
                }
            }
        }
    }

    int totalPages() const {
        int n = 0;
        for (const auto& s : segments) n += s.limit;
        return n;
    }
    int residentPages() const {
        int n = 0;
        for (const auto& [id, dt] : pageTables)
            for (const auto& e : dt.entries)
                if (e) n += e->presentCount();
        return n;
    }

    // Consistency checks between page tables and the frame pool.
    std::vector<std::string> validateConfiguration() {
        std::vector<std::string> problems;
        if ((long)physMem.num_frames * page_size > INT_MAX)
            problems.push_back("physical memory (" + S(physMem.num_frames) + " x " + S(page_size) +
                               " bytes) exceeds the addressable range");
        std::vector<int> seen(physMem.num_frames, 0);
        for (auto& [id, dt] : pageTables)
            for (int d = 0; d < dt.numDirs(); ++d) {
                PageTable* pt = dt.get(d);
                if (!pt) continue;
                for (int p = 0; p < (int)pt->pages.size(); ++p) {
                    const Page& pg = pt->pages[p];
                    if (!pg.present) continue;
                    if (pg.frame_number < 0 || pg.frame_number >= physMem.num_frames) {
                        problems.push_back("S" + S(id) + ":D" + S(d) + ":P" + S(p) + " maps to invalid frame " +
                                           S(pg.frame_number));
                        continue;
                    }
                    if (++seen[pg.frame_number] > 1)
                        problems.push_back("frame " + S(pg.frame_number) + " is mapped by more than one page");
                    const FrameOwner& o = physMem.owner[pg.frame_number];
                    if (o.seg != id || o.dir != d || o.page != p)
                        problems.push_back("frame " + S(pg.frame_number) + " owner record does not match its page");
                }
            }
        if (residentPages() > physMem.num_frames)
            problems.push_back("more resident pages than physical frames");
        return problems;
    }

    TranslationResult translateAddress(int segNum, int pageDir, int pageNum, int offset, AccessType access) {
        TranslationResult r;
        r.logical = formatLogical(segNum, pageDir, pageNum, offset, access);
        r.latency = 1 + rng() % 5;   // simulated translation latency, 1-5 units
        r.start_time = clock;
        int now = ++physMem.time;
        metrics.requests++;

        // 1. Segment table lookup
        if (segNum < 0 || segNum >= (int)segments.size())
            return fail(r, ErrorKind::SEGMENTATION_FAULT,
                        "Invalid segment " + S(segNum) + ", max " + S((int)segments.size() - 1));
        const Segment& seg = segments[segNum];
        DirectoryTable& dt = pageTables.at(segNum);
        r.trace.push_back("Segment " + S(segNum) + ": page directory at " + hexStr(seg.base_address) + ", limit " +
                          S(seg.limit) + " pages, " + protName(seg.protection));

        // 2. Bounds checks on directory index, page number and offset
        if (pageDir < 0 || pageDir >= dt.numDirs())
            return fail(r, ErrorKind::DIRECTORY_FAULT,
                        "Invalid page directory " + S(pageDir) + " for segment " + S(segNum) + ", max " +
                            S(dt.numDirs() - 1));
        int pagesHere = dt.pagesInDir(pageDir);
        if (pageNum < 0 || pageNum >= pagesHere)
            return fail(r, ErrorKind::LIMIT_VIOLATION,
                        "Invalid page " + S(pageNum) + " in directory " + S(pageDir) + " of segment " + S(segNum) +
                            ", max " + S(pagesHere - 1) + " (segment limit " + S(seg.limit) + " pages)");
        if (offset < 0 || offset >= page_size)
            return fail(r, ErrorKind::OFFSET_FAULT,
                        "Invalid offset " + S(offset) + ", max " + S(page_size - 1) + " (page size " + S(page_size) +
                            ")");

        // 3. Segment-level protection
        if (access == WRITE && seg.protection == READ_ONLY)
            return fail(r, ErrorKind::PROTECTION_VIOLATION, "Cannot write to read-only segment " + S(segNum));

        // 4. Page directory -> page table (allocated on first use)
        if (!dt.isAllocated(pageDir)) {
            dt.allocate(pageDir);
            r.dir_allocated = true;
            metrics.dir_allocations++;
            r.trace.push_back("Directory " + S(pageDir) + ": entry was empty -> allocated a new " + S(pagesHere) +
                              "-entry page table");
        } else {
            r.trace.push_back("Directory " + S(pageDir) + ": -> page table with " + S(pagesHere) + " entries");
        }
        PageTable& pt = *dt.get(pageDir);

        // 5. Page table lookup: page protection, presence bit
        Lookup lookup;
        int frame = pt.getFrameNumber(pageNum, now, access, lookup);
        if (lookup == Lookup::INVALID_PAGE)
            return fail(r, ErrorKind::LIMIT_VIOLATION, "Invalid page " + S(pageNum));
        if (lookup == Lookup::PROTECTION)
            return fail(r, ErrorKind::PROTECTION_VIOLATION,
                        "Cannot write to read-only page " + S(pageNum) + " (segment " + S(segNum) + ", directory " +
                            S(pageDir) + ")");
        if (lookup == Lookup::NOT_PRESENT) {
            r.page_fault = true;
            metrics.page_faults++;
            r.trace.push_back("Page " + S(pageNum) + ": present bit = 0 -> PAGE FAULT");
            frame = handlePageFault(segNum, pageDir, pageNum, r);
            r.latency += PAGE_FAULT_PENALTY;
        } else {
            metrics.hits++;
            r.trace.push_back("Page " + S(pageNum) + ": present in frame " + S(frame) + " (" +
                              protName(pt.pages[pageNum].protection) + ")");
        }
        physMem.touch(frame);

        // 6. Physical address
        r.ok = true;
        r.frame = frame;
        r.physical = (long)frame * page_size + offset;
        r.trace.push_back("Physical address = frame " + S(frame) + " x " + S(page_size) + " + offset " + S(offset) +
                          " = " + S(r.physical));
        metrics.successes++;
        return finish(r);
    }

    void printMemoryMap() {
        ui::banner("MEMORY MAP");
        std::cout << "Policy " << ui::bold(policyName(physMem.policy)) << " | Frames " << physMem.num_frames
                  << " | Page size " << page_size << " B | Pages/directory " << pages_per_dir << " | Clock "
                  << clock << "\n\n";
        std::cout << ui::bold("Segment table") << "\n";
        std::cout << "  Seg  Dir base   Limit  Prot  Directory entries (allocated)\n";
        for (size_t i = 0; i < segments.size(); ++i) {
            const DirectoryTable& dt = pageTables.at((int)i);
            std::cout << "  " << std::left << std::setw(5) << i << std::setw(11) << hexStr(segments[i].base_address)
                      << std::setw(7) << segments[i].limit << std::setw(6) << protName(segments[i].protection)
                      << dt.numDirs() << " (" << dt.allocatedTables() << ")" << std::right << "\n";
        }
        for (size_t i = 0; i < segments.size(); ++i) {
            DirectoryTable& dt = pageTables.at((int)i);
            std::cout << "\n" << ui::bold("Segment " + S(i)) << " [" << protName(segments[i].protection) << "]\n";
            for (int d = 0; d < dt.numDirs(); ++d) {
                PageTable* pt = dt.get(d);
                if (!pt) {
                    std::cout << "  Dir " << d << ": " << ui::dim("(not allocated - no page table)") << "\n";
                    continue;
                }
                std::cout << "  Dir " << d << ": page table, " << pt->pages.size() << " entries\n";
                for (size_t p = 0; p < pt->pages.size(); ++p) {
                    const Page& pg = pt->pages[p];
                    std::cout << "    Page " << p << " (vpn " << std::setw(2) << d * pages_per_dir + (int)p << "): ";
                    if (pg.present)
                        std::cout << ui::good("frame " + S(pg.frame_number)) << std::string(pg.frame_number < 10 ? 2 : 1, ' ')
                                  << "present=1 " << protName(pg.protection) << "  last=" << pg.last_access << "\n";
                    else
                        std::cout << ui::dim("--       present=0 ") << protName(pg.protection) << "\n";
                }
            }
        }
        std::cout << "\n" << ui::bold("Physical frames") << "\n";
        for (int f = 0; f < physMem.num_frames; ++f) {
            std::string cell = physMem.free_frames[f] ? "   free    " : ownerName(physMem.owner[f]);
            cell.resize(11, ' ');
            std::cout << "  F" << std::setw(2) << std::setfill('0') << f << std::setfill(' ') << " ["
                      << (physMem.free_frames[f] ? ui::dim(cell) : cell) << "]";
            if (f % 4 == 3 || f == physMem.num_frames - 1) std::cout << "\n";
        }
        printFootprint();
    }

    void printFootprint() {
        int dirEntries = 0, ptEntries = 0;
        for (auto& [id, dt] : pageTables) {
            dirEntries += dt.numDirs();
            ptEntries += dt.allocatedEntries();
        }
        int single = totalPages();
        std::cout << "\nPhysical memory utilization: " << physMem.usedFrames() << "/" << physMem.num_frames
                  << " frames (" << fixed2(physMem.utilization(), 1) << "%)\n";
        std::cout << "Page-table entries: two-level = " << dirEntries << " directory + " << ptEntries
                  << " page-table = " << dirEntries + ptEntries << "  |  single-level would need " << single << "\n";
    }

private:
    int handlePageFault(int seg, int dir, int page, TranslationResult& r) {
        FrameOwner who{seg, dir, page};
        int frame = physMem.allocateFrame(who);
        if (frame >= 0) {
            r.trace.push_back("Loaded page into free frame " + S(frame));
        } else {
            frame = physMem.selectVictim();
            FrameOwner old = physMem.owner[frame];
            pageTables.at(old.seg).get(old.dir)->replacePage(old.page);
            physMem.reassign(frame, who);
            r.replaced = true;
            r.evicted = old;
            metrics.replacements++;
            r.trace.push_back(std::string("Memory full: ") + policyName(physMem.policy) + " evicted " +
                              ownerName(old) + " from frame " + S(frame) + ", loaded page there");
        }
        pageTables.at(seg).get(dir)->setFrame(page, frame, physMem.time);
        return frame;
    }

    TranslationResult fail(TranslationResult& r, ErrorKind kind, const std::string& msg) {
        r.error = kind;
        r.message = msg;
        switch (kind) {
            case ErrorKind::SEGMENTATION_FAULT: metrics.seg_faults++; break;
            case ErrorKind::DIRECTORY_FAULT: metrics.dir_faults++; break;
            case ErrorKind::LIMIT_VIOLATION: metrics.limit_faults++; break;
            case ErrorKind::OFFSET_FAULT: metrics.offset_faults++; break;
            case ErrorKind::PROTECTION_VIOLATION: metrics.prot_faults++; break;
            default: break;
        }
        return finish(r);
    }

    TranslationResult finish(TranslationResult& r) {
        clock += r.latency;
        metrics.total_latency += r.latency;
        timeline.push_back({metrics.requests, r.start_time, r.latency, r.logical, outcomeText(r), r.ok, r.page_fault});
        if (errorLog) {
            if (!r.ok)
                *errorLog << "[t=" << r.start_time << "] #" << metrics.requests << " " << r.logical << "  "
                          << errorName(r.error) << ": " << r.message << "\n";
            else if (r.page_fault)
                *errorLog << "[t=" << r.start_time << "] #" << metrics.requests << " " << r.logical
                          << "  PAGE FAULT (serviced): loaded into frame " << r.frame
                          << (r.replaced ? ", " + std::string(policyName(physMem.policy)) + " evicted " + ownerName(r.evicted)
                                         : std::string(", frame was free"))
                          << "\n";
            errorLog->flush();
        }
        return r;
    }
};

// ----------------------------------------------------------------------------
//  Configuration: interactive prompts or a config file
// ----------------------------------------------------------------------------
//  Config file format (see sample_config.txt):
//      frames 7
//      page_size 1000
//      pages_per_dir 4
//      policy LRU
//      # segID  base    limit  protection  pages...
//      0        0x1000  6      RO          2:RO 5:RO -:RO
//  Page token = <frame or ->:<RO|RW>   ("-" = not present, protection optional)
//  Directories with no listed pages start unallocated.
std::unique_ptr<SegmentTable> loadConfig(const std::string& path, unsigned seed, std::vector<std::string>& errors) {
    std::ifstream in(path);
    if (!in) {
        errors.push_back("cannot open '" + path + "'");
        return nullptr;
    }
    long frames = -1, pageSize = -1, ppd = 4;
    Policy policy = LRU;
    struct SegLine {
        int line;
        std::vector<std::string> tok;
    };
    std::vector<SegLine> segLines;

    std::string raw;
    int lineNo = 0;
    while (std::getline(in, raw)) {
        ++lineNo;
        size_t hash = raw.find('#');
        if (hash != std::string::npos) raw = raw.substr(0, hash);
        std::vector<std::string> tok = tokenize(raw);
        if (tok.empty()) continue;
        std::string key = upper(tok[0]);
        std::string where = "line " + S(lineNo) + ": ";
        if (key == "SEGMENT" || key == "SEG") tok.erase(tok.begin()), key = tok.empty() ? "" : tok[0];
        long dummy;
        if (key == "FRAMES" || key == "PAGE_SIZE" || key == "PAGES_PER_DIR") {
            long v;
            if (tok.size() != 2 || !parseLong(tok[1], v)) {
                errors.push_back(where + "expected '" + tok[0] + " <number>'");
                continue;
            }
            (key == "FRAMES" ? frames : key == "PAGE_SIZE" ? pageSize : ppd) = v;
        } else if (key == "POLICY") {
            if (tok.size() != 2 || !parsePolicy(tok[1], policy)) errors.push_back(where + "policy must be FIFO or LRU");
        } else if (parseLong(key, dummy)) {
            segLines.push_back({lineNo, tok});
        } else {
            errors.push_back(where + "unknown setting '" + tok[0] + "'");
        }
    }

    if (frames < 1 || frames > MAX_FRAMES) errors.push_back("frames must be 1-" + S(MAX_FRAMES));
    if (pageSize < 1 || pageSize > MAX_PAGE_SIZE) errors.push_back("page_size must be 1-" + S(MAX_PAGE_SIZE));
    if (ppd < 1 || ppd > MAX_PAGES_PER_DIR) errors.push_back("pages_per_dir must be 1-" + S(MAX_PAGES_PER_DIR));
    if (frames > 0 && pageSize > 0 && frames * pageSize > INT_MAX)
        errors.push_back("frames x page_size exceeds the addressable range");
    if (segLines.empty()) errors.push_back("no segments defined");
    if (!errors.empty()) return nullptr;

    auto st = std::make_unique<SegmentTable>((int)frames, (int)pageSize, (int)ppd, policy, seed);
    for (const SegLine& sl : segLines) {
        std::string where = "line " + S(sl.line) + ": ";
        long id, base, limit;
        Protection prot;
        if (sl.tok.size() < 4 || !parseLong(sl.tok[0], id) || !parseLong(sl.tok[1], base, 0) ||
            !parseLong(sl.tok[2], limit) || !parseProtection(sl.tok[3], prot)) {
            errors.push_back(where + "expected '<segID> <base> <limit> <RO|RW> [pages...]'");
            continue;
        }
        std::string err;
        if (!st->addSegment((int)id, (int)base, (int)limit, prot, err)) {
            errors.push_back(where + err);
            continue;
        }
        int nPages = (int)sl.tok.size() - 4;
        if (nPages > limit) {
            errors.push_back(where + "segment " + S(id) + " lists " + S(nPages) + " pages but its limit is " + S(limit));
            continue;
        }
        for (int vpn = 0; vpn < nPages; ++vpn) {
            const std::string& t = sl.tok[4 + vpn];
            std::string framePart = t, protPart;
            size_t colon = t.find(':');
            if (colon != std::string::npos) framePart = t.substr(0, colon), protPart = t.substr(colon + 1);
            Protection pageProt = prot;
            if (!protPart.empty() && !parseProtection(protPart, pageProt)) {
                errors.push_back(where + "bad protection in page token '" + t + "'");
                continue;
            }
            if (prot == READ_ONLY) pageProt = READ_ONLY;   // a page cannot be more permissive than its segment
            int d = vpn / (int)ppd, p = vpn % (int)ppd;
            st->pageTables.at((int)id).allocate(d).pages[p].protection = pageProt;
            if (framePart == "-") continue;
            long f;
            if (!parseLong(framePart, f)) {
                errors.push_back(where + "bad frame in page token '" + t + "'");
                continue;
            }
            if (!st->loadPage((int)id, vpn, (int)f, err)) errors.push_back(where + "segment " + S(id) + " page " + S(vpn) + ": " + err);
        }
    }
    if (!errors.empty()) return nullptr;
    return st;
}

void printConfigurationCheck(SegmentTable& st, const std::string& source, unsigned seed) {
    ui::banner("CONFIGURATION CHECK");
    long physBytes = (long)st.physMem.num_frames * st.page_size;
    int total = st.totalPages(), resident = st.residentPages();
    std::cout << "Source            : " << source << "\n";
    std::cout << "Random seed       : " << seed << "  (re-run with --seed " << seed << " to reproduce)\n";
    std::cout << "Physical memory   : " << st.physMem.num_frames << " frames x " << st.page_size << " B = " << physBytes
              << " B\n";
    std::cout << "Segments          : " << st.segments.size() << "  (pages per directory entry: " << st.pages_per_dir
              << ")\n";
    std::cout << "Virtual pages     : " << total << " total, " << resident << " resident at start\n";
    std::cout << "Replacement policy: " << policyName(st.physMem.policy) << "\n";
    if (total > st.physMem.num_frames)
        std::cout << ui::warn("[NOTE] ") << "Virtual pages exceed physical frames (" << fixed2((double)total / st.physMem.num_frames)
                  << "x): demand paging with " << policyName(st.physMem.policy) << " replacement will be used.\n";
    std::vector<std::string> problems = st.validateConfiguration();
    if (problems.empty()) {
        std::cout << ui::good("[OK] ") << "Every resident page maps to a distinct, valid frame (" << resident << " <= "
                  << st.physMem.num_frames << ")\n";
        std::cout << ui::good("[OK] ") << "Page-directory regions do not overlap; addresses fit in range\n";
    } else {
        for (const auto& p : problems) std::cout << ui::bad("[ERROR] ") << p << "\n";
    }
}

std::unique_ptr<SegmentTable> interactiveSetup(unsigned seed, std::string& source) {
    ui::banner("SETUP");
    while (readYesNo("Load configuration from a file?", false)) {
        std::string path = readLine("Config file path [sample_config.txt]: ");
        if (path.empty()) path = "sample_config.txt";
        std::vector<std::string> errors;
        auto st = loadConfig(path, seed, errors);
        if (st) {
            source = "file " + path;
            return st;
        }
        std::cout << ui::bad("Configuration rejected:") << "\n";
        for (const auto& e : errors) std::cout << "  - " << e << "\n";
    }
    while (true) {
        int frames = (int)readInt("Number of physical frames", 1, MAX_FRAMES, 16);
        int pageSize = (int)readInt("Page size in bytes", 1, MAX_PAGE_SIZE, 1000);
        int numSegments = (int)readInt("Number of segments", 1, MAX_SEGMENTS, 4);
        int ppd = (int)readInt("Pages per page-directory entry (2nd-level table size)", 1, MAX_PAGES_PER_DIR, 4);
        Policy policy = LRU;
        while (true) {
            std::string p = readLine("Replacement policy FIFO or LRU [LRU]: ");
            if (p.empty() || parsePolicy(p, policy)) break;
            std::cout << ui::bad("  Please type FIFO or LRU.") << "\n";
        }
        if ((long)frames * pageSize > INT_MAX) {
            std::cout << ui::bad("  frames x page size is too large (max " + S(INT_MAX) + " bytes). Try again.") << "\n";
            continue;
        }
        auto st = std::make_unique<SegmentTable>(frames, pageSize, ppd, policy, seed);
        st->initRandom(numSegments);
        source = "interactive (random segments)";
        return st;
    }
}

// ----------------------------------------------------------------------------
//  Application: menu actions
// ----------------------------------------------------------------------------
struct LogicalAddress {
    int seg, dir, page, offset;
    AccessType access;
};

bool parseAddress(const std::string& line, LogicalAddress& a, std::string& err) {
    std::vector<std::string> t = tokenize(line);
    if (t.size() != 5) {
        err = "expected 5 values: seg dir page offset access";
        return false;
    }
    long v[4];
    for (int i = 0; i < 4; ++i)
        if (!parseLong(t[i], v[i]) || v[i] < INT_MIN || v[i] > INT_MAX) {
            err = "'" + t[i] + "' is not a whole number";
            return false;
        }
    if (!parseAccess(t[4], a.access)) {
        err = "access must be R/W (or 0=read, 1=write)";
        return false;
    }
    a.seg = (int)v[0], a.dir = (int)v[1], a.page = (int)v[2], a.offset = (int)v[3];
    return true;
}

struct RunRecord {
    std::string name;
    long requests;
    long latency;
};

struct App {
    std::unique_ptr<SegmentTable> st;
    unsigned seed;
    std::string source;
    std::ofstream errorLog;
    std::vector<RunRecord> runs;
};

void printSummary(std::ostream& out, const std::string& title, const Metrics& m, SegmentTable& st, bool colored) {
    auto c = [&](std::string (*f)(const std::string&), const std::string& s) { return colored ? f(s) : s; };
    out << "\n" << c(ui::head, "---------- " + title + " summary ----------") << "\n";
    out << "Translations requested : " << m.requests << "\n";
    out << "Successful             : " << c(ui::good, S(m.successes)) << "  (" << pct(m.successes, m.requests) << ")\n";
    out << "Faults / errors        : " << c(ui::bad, S(m.errors())) << "  (" << pct(m.errors(), m.requests) << ")\n";
    out << "    Segmentation faults   : " << m.seg_faults << "\n";
    out << "    Directory faults      : " << m.dir_faults << "\n";
    out << "    Limit violations      : " << m.limit_faults << "\n";
    out << "    Offset faults         : " << m.offset_faults << "\n";
    out << "    Protection violations : " << m.prot_faults << "\n";
    out << "Page faults (serviced) : " << c(ui::warn, S(m.page_faults)) << "  (rate " << pct(m.page_faults, m.requests)
        << " of requests, " << pct(m.page_faults, m.successes) << " of valid references)\n";
    out << "Page replacements      : " << m.replacements << "  (" << policyName(st.physMem.policy) << ")\n";
    out << "Page tables allocated  : " << m.dir_allocations << " on demand\n";
    out << "Memory utilization     : " << st.physMem.usedFrames() << "/" << st.physMem.num_frames << " frames ("
        << fixed2(st.physMem.utilization(), 1) << "%)\n";
    out << "Average latency        : " << fixed2(m.avgLatency()) << " units  (total " << m.total_latency << ")\n";
}

void recordRun(App& app, const std::string& name, const Metrics& m) {
    if (m.requests == 0) return;
    app.runs.push_back({name, m.requests, m.total_latency});
    bool exists = std::ifstream(LATENCY_HISTORY_FILE).good();
    std::ofstream h(LATENCY_HISTORY_FILE, std::ios::app);
    if (!exists) h << "timestamp,run,requests,total_latency,avg_latency\n";
    h << nowString() << "," << name << "," << m.requests << "," << m.total_latency << "," << fixed2(m.avgLatency()) << "\n";
}

void printTranslation(const TranslationResult& r, long id) {
    std::cout << "\n  " << ui::bold("#" + S(id) + " " + r.logical) << "\n";
    for (const auto& t : r.trace) std::cout << "     - " << t << "\n";
    if (r.ok) {
        std::string tag = r.page_fault ? ui::warn("PAGE FAULT serviced") : ui::good("HIT");
        std::cout << "     => " << tag << "  Physical address: " << ui::bold(S(r.physical)) << "   latency " << r.latency
                  << "\n";
    } else {
        std::cout << "     => " << ui::bad(std::string(errorName(r.error)) + ": " + r.message) << "   latency "
                  << r.latency << "\n";
    }
}

void runManual(App& app) {
    SegmentTable& st = *app.st;
    ui::banner("MANUAL ADDRESS TRANSLATION");
    std::cout << "Enter: seg dir page offset access   (access: R/W or 0=read, 1=write)\n";
    std::cout << "Example: 1 0 2 250 R     Commands: m = memory map, q or -1 = back to menu\n";
    Metrics before = st.metrics;
    while (true) {
        std::string line = readLine("\naddr> ");
        std::string u = upper(line);
        if (u == "Q" || u == "-1" || u == "QUIT" || u == "EXIT") break;
        if (u == "M") {
            st.printMemoryMap();
            continue;
        }
        if (line.empty()) continue;
        LogicalAddress a;
        std::string err;
        if (!parseAddress(line, a, err)) {
            std::cout << ui::bad("  Input error: " + err) << "\n";
            continue;
        }
        TranslationResult r = st.translateAddress(a.seg, a.dir, a.page, a.offset, a.access);
        printTranslation(r, st.metrics.requests);
    }
    Metrics diff = st.metrics - before;
    if (diff.requests) {
        printSummary(std::cout, "Manual session", diff, st, true);
        recordRun(app, "manual", diff);
    }
}

LogicalAddress randomValidAddress(SegmentTable& st, std::mt19937& g) {
    LogicalAddress a;
    a.seg = (int)(g() % st.segments.size());
    int vpn = (int)(g() % st.segments[a.seg].limit);
    a.dir = vpn / st.pages_per_dir;
    a.page = vpn % st.pages_per_dir;
    a.offset = (int)(g() % st.page_size);
    a.access = (g() % 2) ? WRITE : READ;
    return a;
}

LogicalAddress randomInvalidAddress(SegmentTable& st, std::mt19937& g) {
    LogicalAddress a = randomValidAddress(st, g);
    DirectoryTable& dt = st.pageTables.at(a.seg);
    switch (g() % 4) {
        case 0: a.seg = (int)st.segments.size() + (int)(g() % 3); break;      // no such segment
        case 1: a.dir = dt.numDirs() + (int)(g() % 2); break;                 // directory out of range
        case 2: a.page = dt.pagesInDir(a.dir) + (int)(g() % 2); break;        // beyond segment limit
        default: a.offset = st.page_size + (int)(g() % 100); break;           // offset too big
    }
    return a;
}

void runStress(App& app) {
    SegmentTable& st = *app.st;
    ui::banner("STRESS TEST");
    int count = (int)readInt("How many random addresses", 1, 100000, 100);
    double ratio = readDouble("Fraction of in-range (valid) addresses 0.0-1.0", 0.0, 1.0, 0.70);

    std::ofstream out(RESULTS_FILE);
    out << "==================== Stress Test Results ====================\n";
    out << "Generated : " << nowString() << "\n";
    out << "Config    : " << st.physMem.num_frames << " frames, page size " << st.page_size << ", " << st.segments.size()
        << " segments, " << st.pages_per_dir << " pages/dir, " << policyName(st.physMem.policy) << ", seed " << app.seed
        << "\n";
    out << "Requested : " << count << " addresses, valid ratio " << fixed2(ratio) << "\n";
    out << "Format    : # [start-end] (seg,dir,page,offset,access) outcome  latency\n";
    out << "-------------------------------------------------------------\n";

    Metrics before = st.metrics;
    std::uniform_real_distribution<double> coin(0.0, 1.0);
    int generatedValid = 0;
    for (int i = 0; i < count; ++i) {
        bool valid = coin(st.rng) < ratio;
        generatedValid += valid;
        LogicalAddress a = valid ? randomValidAddress(st, st.rng) : randomInvalidAddress(st, st.rng);
        TranslationResult r = st.translateAddress(a.seg, a.dir, a.page, a.offset, a.access);
        out << std::setw(4) << i + 1 << " [" << std::setw(4) << r.start_time << "-" << std::setw(4)
            << r.start_time + r.latency << "] " << std::left << std::setw(22) << r.logical << std::right << outcomeText(r)
            << "  lat=" << r.latency << "\n";
    }
    Metrics diff = st.metrics - before;
    out << "-------------------------------------------------------------\n";
    out << "Generated " << generatedValid << " in-range and " << count - generatedValid << " out-of-range addresses\n";
    printSummary(out, "Stress test", diff, st, false);
    out.close();

    std::cout << "Generated " << generatedValid << " in-range and " << count - generatedValid
              << " out-of-range addresses.\n";
    printSummary(std::cout, "Stress test", diff, st, true);
    std::cout << "\nFull per-address log written to " << ui::bold(RESULTS_FILE) << ", fault details in "
              << ui::bold(ERROR_LOG_FILE) << "\n";
    recordRun(app, "stress", diff);
}

void runBatch(App& app) {
    SegmentTable& st = *app.st;
    ui::banner("BATCH PROCESSING");
    std::string path = readLine("Batch file path [batch_addresses.txt]: ");
    if (path.empty()) path = "batch_addresses.txt";
    std::ifstream in(path);
    if (!in) {
        std::cout << ui::bad("Cannot open '" + path + "'") << "\n";
        return;
    }
    std::ofstream out(BATCH_RESULTS_FILE);
    out << "Batch file: " << path << "   (" << nowString() << ")\n";
    Metrics before = st.metrics;
    int malformed = 0, lineNo = 0;
    std::string raw;
    while (std::getline(in, raw)) {
        ++lineNo;
        size_t hash = raw.find('#');
        if (hash != std::string::npos) raw = raw.substr(0, hash);
        if (trim(raw).empty()) continue;
        LogicalAddress a;
        std::string err;
        if (!parseAddress(raw, a, err)) {
            ++malformed;
            std::string msg = "line " + S(lineNo) + ": skipped (" + err + ")";
            std::cout << "  " << ui::bad(msg) << "\n";
            out << msg << "\n";
            continue;
        }
        TranslationResult r = st.translateAddress(a.seg, a.dir, a.page, a.offset, a.access);
        std::string text = r.logical;
        text.resize(22, ' ');
        std::string outcome = outcomeText(r);
        std::cout << "  " << std::setw(3) << st.metrics.requests - before.requests << ". " << text
                  << (r.ok ? (r.page_fault ? ui::warn(outcome) : ui::good(outcome)) : ui::bad(outcome)) << "\n";
        out << text << outcome << "  lat=" << r.latency << "\n";
    }
    Metrics diff = st.metrics - before;
    if (malformed) std::cout << ui::warn("  " + S(malformed) + " malformed line(s) skipped") << "\n";
    printSummary(std::cout, "Batch", diff, st, true);
    printSummary(out, "Batch", diff, st, false);
    std::cout << "\nBatch results written to " << ui::bold(BATCH_RESULTS_FILE) << "\n";
    recordRun(app, "batch", diff);
}

void showTimeline(App& app) {
    SegmentTable& st = *app.st;
    ui::banner("TRANSLATION TIMELINE (Gantt)");
    if (st.timeline.empty()) {
        std::cout << "No translations yet.\n";
        return;
    }
    int n = (int)readInt("How many of the most recent translations to show", 1, (long)st.timeline.size(),
                         std::min<long>(20, (long)st.timeline.size()));
    std::cout << "Legend: " << ui::good("#") << " translation  " << ui::warn("~") << " page-fault service  "
              << ui::bad("x") << " rejected\n\n";
    for (size_t i = st.timeline.size() - n; i < st.timeline.size(); ++i) {
        const TimelineEntry& e = st.timeline[i];
        int base = e.page_fault ? e.latency - PAGE_FAULT_PENALTY : e.latency;
        std::string bar = e.ok ? ui::good(std::string(base, '#')) + ui::warn(std::string(e.latency - base, '~'))
                               : ui::bad(std::string(e.latency, 'x'));
        std::string pad(15 - e.latency, ' ');
        std::cout << "Time " << std::setw(4) << e.start << "-" << std::left << std::setw(4) << e.start + e.latency
                  << std::right << " |" << bar << pad << "| #" << std::left << std::setw(4) << e.id << std::setw(20)
                  << e.logical << std::right << e.outcome << "\n";
    }
}

void showMetrics(App& app) {
    SegmentTable& st = *app.st;
    ui::banner("METRICS");
    printSummary(std::cout, "Whole session", st.metrics, st, true);
    st.printFootprint();
    if (!app.runs.empty()) {
        std::cout << "\n" << ui::bold("Runs this session") << "\n";
        for (size_t i = 0; i < app.runs.size(); ++i)
            std::cout << "  " << i + 1 << ". " << std::left << std::setw(8) << app.runs[i].name << std::right
                      << std::setw(6) << app.runs[i].requests << " translations, avg latency "
                      << fixed2((double)app.runs[i].latency / app.runs[i].requests) << "\n";
    }
    std::ifstream h(LATENCY_HISTORY_FILE);
    std::string line;
    long runs = 0, reqs = 0, lat = 0;
    std::getline(h, line);   // header
    while (std::getline(h, line)) {
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string cell;
        while (std::getline(ss, cell, ',')) f.push_back(cell);
        long r, l;
        if (f.size() >= 4 && parseLong(f[2], r) && parseLong(f[3], l)) ++runs, reqs += r, lat += l;
    }
    if (runs)
        std::cout << "\nAverage latency across all " << runs << " recorded runs (" << LATENCY_HISTORY_FILE
                  << "): " << ui::bold(fixed2((double)lat / reqs)) << " units over " << reqs << " translations\n";
}

void compareFifoLru(App& app) {
    SegmentTable& base = *app.st;
    ui::banner("FIFO vs LRU COMPARISON");
    std::cout << "Each workload is replayed from the CURRENT memory state on a copy that uses FIFO\n"
                 "and a copy that uses LRU. All references are valid reads, so only paging differs.\n";
    int refs = (int)readInt("References per workload", 10, 200000, 1000);

    std::vector<std::pair<int, int>> allPages;   // (segment, vpn)
    for (size_t s = 0; s < base.segments.size(); ++s)
        for (int v = 0; v < base.segments[s].limit; ++v) allPages.push_back({(int)s, v});
    std::mt19937 g(base.rng());
    std::shuffle(allPages.begin(), allPages.end(), g);
    int frames = base.physMem.num_frames;
    int hotSize = std::max(1, std::min((int)allPages.size(), frames * 3 / 4));
    int loopSize = std::min((int)allPages.size(), frames + 1);

    struct Workload {
        std::string name;
        std::vector<std::pair<int, int>> refs;
    };
    std::vector<Workload> workloads(3);
    workloads[0].name = "Uniform random";
    workloads[1].name = "Locality 80/20";
    workloads[2].name = "Loop over " + S(loopSize) + " pages";
    for (int i = 0; i < refs; ++i) {
        workloads[0].refs.push_back(allPages[g() % allPages.size()]);
        bool hot = g() % 100 < 80;
        workloads[1].refs.push_back(hot ? allPages[g() % hotSize] : allPages[g() % allPages.size()]);
        workloads[2].refs.push_back(allPages[i % loopSize]);
    }

    std::cout << "\n" << std::left << std::setw(24) << "Workload" << std::setw(20) << "FIFO faults" << std::setw(20)
              << "LRU faults" << "Fewer faults" << std::right << "\n";
    std::cout << std::string(76, '-') << "\n";
    for (const Workload& w : workloads) {
        long faults[2];
        for (int p = 0; p < 2; ++p) {
            SegmentTable copy = base;
            copy.errorLog = nullptr;
            copy.physMem.policy = p == 0 ? FIFO : LRU;
            copy.metrics = Metrics{};
            for (auto [s, v] : w.refs) copy.translateAddress(s, v / copy.pages_per_dir, v % copy.pages_per_dir, 0, READ);
            faults[p] = copy.metrics.page_faults;
        }
        std::string winner = faults[0] == faults[1] ? "tie" : faults[0] < faults[1] ? "FIFO" : "LRU";
        std::cout << std::left << std::setw(24) << w.name << std::setw(20)
                  << (S(faults[0]) + " (" + pct(faults[0], refs) + ")") << std::setw(20)
                  << (S(faults[1]) + " (" + pct(faults[1], refs) + ")") << winner << std::right << "\n";
    }
    std::cout << "\n" << frames << " frames, " << allPages.size() << " virtual pages, hot set = " << hotSize
              << " pages. (The live simulator state is unchanged.)\n";
}

void pageSizeAnalysis(App& app) {
    SegmentTable& base = *app.st;
    ui::banner("PAGE-SIZE ANALYSIS");
    std::cout << "Simulates the same program (same segment sizes in bytes, same physical memory in\n"
                 "bytes, same byte-level reference trace) under two page sizes, from a cold start.\n";
    int sizes[2];
    sizes[0] = (int)readInt("First page size", 16, MAX_PAGE_SIZE, 1000);
    sizes[1] = (int)readInt("Second page size", 16, MAX_PAGE_SIZE, 4000);
    int refs = (int)readInt("Number of references", 10, 200000, 2000);

    long largest = std::max(sizes[0], sizes[1]);
    long defBytes = std::max<long>((long)base.physMem.num_frames * base.page_size, 4 * largest);
    defBytes = (defBytes + largest - 1) / largest * largest;
    long physBytes = readInt("Physical memory in bytes (same for both)", largest, INT_MAX, defBytes);
    std::mt19937 g(base.rng());
    std::vector<long> segBytes;
    long totalBytes = 0;
    for (const Segment& s : base.segments) {
        long b = std::max<long>(1, (long)s.limit * base.page_size - (long)(g() % base.page_size));
        segBytes.push_back(b);
        totalBytes += b;
    }
    // Byte-level trace: 80% of references fall in the first 30% of a segment (hot region).
    std::vector<std::pair<int, long>> trace;
    for (int i = 0; i < refs; ++i) {
        long pick = (long)(g() % totalBytes);
        int s = 0;
        while (pick >= segBytes[s]) pick -= segBytes[s++];
        long hotLen = std::max<long>(1, segBytes[s] * 3 / 10);
        long off = (g() % 100 < 80) ? (long)(g() % hotLen) : (long)(g() % segBytes[s]);
        trace.push_back({s, off});
    }

    std::cout << "\nSegment sizes (bytes):";
    for (size_t i = 0; i < segBytes.size(); ++i) std::cout << "  S" << i << "=" << segBytes[i];
    std::cout << "\nPhysical memory: " << physBytes << " bytes, policy " << policyName(base.physMem.policy)
              << ", pages/dir " << base.pages_per_dir << "\n\n";

    struct Row {
        long frames, pages, pteSingle, ptTwoLevel, frag, faults, replacements;
        double avgLat;
    } row[2];
    for (int k = 0; k < 2; ++k) {
        int ps = sizes[k];
        long frames = std::max<long>(1, physBytes / ps);
        SegmentTable t((int)std::min<long>(frames, MAX_FRAMES), ps, base.pages_per_dir, base.physMem.policy, app.seed);
        Row& r = row[k];
        r.frames = t.physMem.num_frames;
        r.pages = 0;
        r.frag = 0;
        for (size_t i = 0; i < segBytes.size(); ++i) {
            int pages = (int)((segBytes[i] + ps - 1) / ps);
            std::string err;
            t.addSegment((int)i, 0x1000 * ((int)i + 1), std::min(pages, MAX_PAGES_PER_SEGMENT), base.segments[i].protection, err);
            r.pages += pages;
            r.frag += (long)pages * ps - segBytes[i];
        }
        for (auto [s, off] : trace) {
            int vpn = (int)(off / ps);
            t.translateAddress(s, vpn / t.pages_per_dir, vpn % t.pages_per_dir, (int)(off % ps), READ);
        }
        r.pteSingle = r.pages;
        r.ptTwoLevel = 0;
        for (auto& [id, dt] : t.pageTables) r.ptTwoLevel += dt.numDirs() + dt.allocatedEntries();
        r.faults = t.metrics.page_faults;
        r.replacements = t.metrics.replacements;
        r.avgLat = t.metrics.avgLatency();
    }
    auto line = [&](const std::string& label, const std::string& a, const std::string& b) {
        std::cout << "  " << std::left << std::setw(40) << label << std::setw(18) << a << b << std::right << "\n";
    };
    line("", "page size " + S(sizes[0]), "page size " + S(sizes[1]));
    std::cout << "  " << std::string(74, '-') << "\n";
    line("Physical frames", S(row[0].frames), S(row[1].frames));
    line("Virtual pages (= single-level PTEs)", S(row[0].pteSingle), S(row[1].pteSingle));
    line("Two-level entries actually allocated", S(row[0].ptTwoLevel), S(row[1].ptTwoLevel));
    line("Page-table memory (single-level)", S(row[0].pteSingle * PTE_SIZE_BYTES) + " B", S(row[1].pteSingle * PTE_SIZE_BYTES) + " B");
    line("Internal fragmentation", S(row[0].frag) + " B", S(row[1].frag) + " B");
    line("Page faults", S(row[0].faults) + " (" + pct(row[0].faults, refs) + ")", S(row[1].faults) + " (" + pct(row[1].faults, refs) + ")");
    line("Replacements", S(row[0].replacements), S(row[1].replacements));
    line("Average latency (units)", fixed2(row[0].avgLat), fixed2(row[1].avgLat));
    std::cout << "\n(Page-fault service adds " << PAGE_FAULT_PENALTY << " units. The live simulator state is unchanged.)\n";
}

void changePolicy(App& app) {
    Policy p = app.st->physMem.policy;
    std::string in = readLine(std::string("Replacement policy FIFO or LRU [") + policyName(p) + "]: ");
    if (!in.empty() && !parsePolicy(in, p)) {
        std::cout << ui::bad("Unknown policy; unchanged.") << "\n";
        return;
    }
    app.st->physMem.policy = p;
    std::cout << "Replacement policy is now " << ui::bold(policyName(p)) << "\n";
}

void writeLogHeader(App& app) {
    app.errorLog << "=== Error / fault log - " << nowString() << " - " << app.source << ", "
                 << app.st->physMem.num_frames << " frames, page size " << app.st->page_size << ", "
                 << policyName(app.st->physMem.policy) << ", seed " << app.seed << " ===\n";
    app.errorLog.flush();
}

void printUsage() {
    std::cout << "Usage: memory_simulator [--seed N] [--config FILE] [--color | --no-color]\n"
                 "  --seed N       seed the random generator (reproducible runs)\n"
                 "  --config FILE  load memory configuration from FILE instead of prompting\n"
                 "  --no-color     disable ANSI colors\n"
                 "  --color        force ANSI colors (e.g. when output is piped through sed)\n";
}

int main(int argc, char** argv) {
    unsigned seed = (unsigned)std::chrono::system_clock::now().time_since_epoch().count();
    std::string configPath;
    bool noColor = std::getenv("NO_COLOR") != nullptr;
    bool forceColor = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        long v;
        if (a == "--seed" && i + 1 < argc && parseLong(argv[i + 1], v)) seed = (unsigned)v, ++i;
        else if (a == "--config" && i + 1 < argc) configPath = argv[++i];
        else if (a == "--no-color") noColor = true;
        else if (a == "--color") forceColor = true;
        else if (a == "--help" || a == "-h") { printUsage(); return 0; }
        else { std::cout << "Unknown argument: " << a << "\n"; printUsage(); return 1; }
    }
    ui::color = forceColor || (!noColor && ISATTY(FILENO(stdout)));
    ui::echoInput = !ISATTY(FILENO(stdin));
    std::srand(seed);

    App app;
    app.seed = seed;
    app.errorLog.open(ERROR_LOG_FILE);

    ui::banner("CS375 A9 - Segmented, Paged Memory Simulator");
    try {
        if (!configPath.empty()) {
            std::vector<std::string> errors;
            app.st = loadConfig(configPath, seed, errors);
            if (app.st) {
                app.source = "file " + configPath;
            } else {
                std::cout << ui::bad("Configuration file rejected:") << "\n";
                for (const auto& e : errors) std::cout << "  - " << e << "\n";
            }
        }
        if (!app.st) app.st = interactiveSetup(seed, app.source);
        app.st->errorLog = &app.errorLog;
        writeLogHeader(app);
        printConfigurationCheck(*app.st, app.source, seed);

        while (true) {
            ui::banner("MAIN MENU");
            std::cout << "  1) Translate addresses manually\n"
                         "  2) Stress test: random addresses  -> results.txt\n"
                         "  3) Batch: process addresses from a file\n"
                         "  4) Show memory map\n"
                         "  5) Show metrics & latency history\n"
                         "  6) Show translation timeline (Gantt)\n"
                         "  7) Compare FIFO vs LRU\n"
                         "  8) Page-size analysis (e.g. 1000 vs 4000)\n"
                         "  9) Change replacement policy (now "
                      << policyName(app.st->physMem.policy)
                      << ")\n"
                         " 10) Reconfigure memory (new setup)\n"
                         "  0) Exit\n";
            long choice = readInt("choice", 0, 10, 0);
            switch (choice) {
                case 1: runManual(app); break;
                case 2: runStress(app); break;
                case 3: runBatch(app); break;
                case 4: app.st->printMemoryMap(); break;
                case 5: showMetrics(app); break;
                case 6: showTimeline(app); break;
                case 7: compareFifoLru(app); break;
                case 8: pageSizeAnalysis(app); break;
                case 9: changePolicy(app); break;
                case 10:
                    app.st = interactiveSetup(seed, app.source);
                    app.st->errorLog = &app.errorLog;
                    writeLogHeader(app);
                    printConfigurationCheck(*app.st, app.source, seed);
                    break;
                default:
                    std::cout << "\nGoodbye. Final utilization: " << fixed2(app.st->physMem.utilization(), 1)
                              << "%, average latency " << fixed2(app.st->metrics.avgLatency()) << " units.\n";
                    return 0;
            }
        }
    } catch (const InputEnded&) {
        std::cout << "Input ended - exiting.\n";
    }
    return 0;
}
