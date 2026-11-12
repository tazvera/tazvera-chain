#include <iostream>
#include "taz/chain.hpp"

int main() {
    taz::Chain tazvera_chain;
    std::cout << "Tazvera Chain Engine Initialized Successfully! Height: " << tazvera_chain.height() << std::endl;
    return 0;
}
