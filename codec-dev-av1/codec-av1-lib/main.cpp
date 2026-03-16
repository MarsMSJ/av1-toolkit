#include <iostream>
#include "film_grain.hpp"
#include "segmentation.hpp"

int main() { 
    av1::FilmGrainParams grain_params;
    grain_params.apply_grain = true;

    av1::SegmentationData seg_data;
    seg_data.enabled = true;

    std::cout << "AV1 Codec C++ Library initialized." << std::endl;
    std::cout << "Film Grain Applied: " << grain_params.apply_grain << std::endl;
    std::cout << "Segmentation Enabled: " << seg_data.enabled << std::endl;

    return 0; 
}