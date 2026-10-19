// SRO PK2 Studio — GUI entry point.
#include "app.hpp"

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    return pk2::run_app(hInstance, nCmdShow);
}
