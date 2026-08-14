# MiP Dashboard
A web-based dashboard for the WowWee MiP robot, running on the MiP Power Up - D1 mini library. This interface allows you to control the MiP's LEDs and sounds, configure WiFi, view system information, and display a world clock and weather data from OpenWeatherMap. The MiP's LEDs can dynamically react to weather conditions.

## MiP Robot Web Dashboard for ESP8266

This Arduino sketch hosts a web dashboard on an ESP8266 (like a Wemos D1 Mini) to control and interact with a WowWee MiP robot equipped with the [MiP Power Up](https://github.com/Tiogaplanet/MiP_Power_Up) board.

The web interface provides a modern, responsive dashboard with multiple tabs for easy control and monitoring.

### Features

- **System Info**: View MiP hardware/software versions, library version, and ESP8266 network details.
- **Control**: Manually control the MiP's head (eye) LEDs, chest LED (color and blinking), and play various sounds.
- **WiFi Manager**: Scan for and connect to nearby WiFi access points.
- **World Clock**: A world clock with automatic timezone detection from your browser and an alarm clock feature to wake up your MiP.
- **Weather**: View current weather for a configurable city using the OpenWeatherMap API. The MiP's LEDs will automatically reflect the weather conditions (chest color for temperature, eye animation for rain).

### Requirements

1.  **Hardware**:
    *   A WowWee MiP robot modified with the [MiP Power Up](https://github.com/Tiogaplanet/MiP_Power_Up) board.
    *   An ESP8266-based board, such as the Wemos D1 Mini.

2.  **Arduino Libraries**:
    *   [MiP Power Up - D1 mini library](https://github.com/Tiogaplanet/MPU_D1_mini_lib)
    *   [ArduinoJson](https://arduinojson.org/) (can be installed via the Arduino Library Manager)

### Setup and Installation

1.  **Clone the Repository**:
    ```sh
    git clone <your-repository-url>
    cd <your-repository-folder>
    ```

2.  **Install Libraries**:
    Open the Arduino IDE and install the required libraries using the Library Manager (`Sketch` → `Include Library` → `Manage Libraries...`).

3.  **Create `secrets.h`**:
    In the same directory as `Dashboard.ino`, create a new file named `secrets.h`. This file will store your sensitive information and is ignored by Git. Add the following content, replacing the placeholder values with your own:

    ```cpp
    #pragma once

    // Your WiFi network credentials
    const char* SECRET_SSID = "YOUR_WIFI_SSID";
    const char* SECRET_PASS = "YOUR_WIFI_PASSWORD";

    // Your OpenWeatherMap API Key
    // Get one for free at https://openweathermap.org/api
    const char* SECRET_OWM_API_KEY = "YOUR_OWM_API_KEY";
    ```

4.  **Upload the Sketch**:
    Open `Dashboard.ino` in the Arduino IDE, select your ESP8266 board and port, and upload the sketch.

### Usage

Once the ESP8266 connects to your WiFi network, it will print its IP address to the Serial Monitor. Open a web browser and navigate to `http://<mip-ip>/` or `http://mip-dashboard.local/` to access the dashboard.

If the board fails to connect to WiFi, it will start its own Access Point named "MiP-Dashboard" with the password "miprobot". Connect to this network and use the WiFi tab to configure a connection to your home network.
