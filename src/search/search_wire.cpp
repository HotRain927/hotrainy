#include "hr/search/search_wire.h"

#include <windows.h>

#include "hr/app/app_context.h"
#include "hr/search/search.h"

namespace hr {

void searchWire() {
    search().onAutoClosed = [](HWND hwnd) { dialogDismissed() = hwnd; };
}

}  // namespace hr
