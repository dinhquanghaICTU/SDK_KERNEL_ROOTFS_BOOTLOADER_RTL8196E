#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

int main()
{
    constexpr const char* led =
        "/sys/class/leds/status/brightness";

    while (true) {
        {
            std::ofstream file(led);
            if (!file) {
                std::cerr << "Cannot open " << led << '\n';
                return 1;
            }
            file << "1\n";
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        {
            std::ofstream file(led);
            file << "0\n";
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}