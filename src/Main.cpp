#include "UciEngine.hpp"

#include "probe.h"

int main() {
    Stockfish::Probe::init("nn-71d6d32cb962.nnue", "nn-b1a57edbea57.nnue");

    UciEngine engine;
    engine.run();
    return 0;
}