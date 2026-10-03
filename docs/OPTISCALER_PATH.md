# OptiScaler path

Do not make OptiScaler a C++ plugin API.

FSRScale deliberately makes the same FFX API calls a game would make:

    ffxCreateContext
    ffxDispatch
    ffxDestroyContext

The executable imports the AMD FFX runtime. OptiScaler is installed as its normal
DXGI proxy (`dxgi.dll`) in the FSRScale directory. Its in-process hooks then see
the FFX calls and can replace the provider/backend.

This gives the desired relationship:

    FSRScale = host/capture/presentation
    FSR 3.1 = contract
    OptiScaler = interception/backend replacement

The app remains usable without OptiScaler, which is useful for isolating capture/FSR
problems from OptiScaler problems.
