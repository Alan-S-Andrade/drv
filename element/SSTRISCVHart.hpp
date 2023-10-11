#include <RISCVHart.hpp>
namespace SST {
namespace Drv {

class RISCVSimHart : public RISCVHart {
public:
    RISCVSimHart()
        : RISCVHart(),
          _ready(true),
          _exit(false) {}
    virtual ~RISCVSimHart() {}

    int & ready() { return _ready; }
    int   ready() const { return _ready; }

    int & exit() { return _exit; }
    int   exit() const { return _exit; }
    
    int _ready;
    int _exit;
};

}
}
