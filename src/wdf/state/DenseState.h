#pragma once

#include <algorithm>
#include <vector>

namespace wdf::state {

class DenseState {
public:
    std::vector<double> capState_;
    std::vector<double> indState_;
    std::vector<double> isCur_;

    std::vector<double> b_;
    std::vector<double> w_;
    std::vector<double> node_;
    std::vector<double> nodeSafe_;

    void resetWaves() {
        std::fill(capState_.begin(), capState_.end(), 0.0);
        std::fill(indState_.begin(), indState_.end(), 0.0);
        std::fill(isCur_.begin(), isCur_.end(), 0.0);
        std::fill(b_.begin(), b_.end(), 0.0);
        std::fill(w_.begin(), w_.end(), 0.0);
        std::fill(node_.begin(), node_.end(), 0.0);
        std::fill(nodeSafe_.begin(), nodeSafe_.end(), 0.0);
    }
};

}  // namespace wdf::state
