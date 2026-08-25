#pragma once

#include <sstream>

#include "SearchController.hpp"

class UciEngine {
public:
    void run();

private:
    void handle_position(std::stringstream& command);
    void handle_go(std::stringstream& command);

    chess::Board board;
    int num_threads = 1;
    SearchController search;
};
