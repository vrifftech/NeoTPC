#pragma once

#include <stdexcept>

namespace neotpc::texture {

class TextureError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

} // namespace neotpc::texture
