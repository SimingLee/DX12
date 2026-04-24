#include "engine/Application.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    engine::Application application;
    if (!application.Initialize())
    {
        return -1;
    }

    return application.Run();
}
