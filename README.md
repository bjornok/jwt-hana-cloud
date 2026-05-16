# SAP HANA Cloud & Microsoft Entra ID JWT Integration

This project provides sample code (C++ and Python) to demonstrate how to authenticate and connect to **SAP HANA Cloud** using **Microsoft Entra ID** (formerly Azure AD) JWT tokens.

## Project Overview

The samples illustrate the **Device Code Flow** to obtain a JWT token from Entra ID and use that token to establish an ODBC connection to SAP HANA Cloud. This approach allows for secure, scalable authentication mapped to user email addresses.

## Prerequisites

- **SAP HANA Client:** Installed on your system (provides the `HDBODBC` driver).
- **Entra ID App Registration:** Configured as described in [SAP_HANA_ODBC_SETUP.md](./SAP_HANA_ODBC_SETUP.md).
- **ODBC DSN:** Configured in your `odbc.ini`.

## File Structure

- `hana_odbc_sample.cpp`: A comprehensive C++ sample that supports:
    - Manual Device Code Flow using `libcurl` and `nlohmann/json`.
    - Basic Authentication (Username/Password).
    - Modern Device Code Flow using the **Azure Identity SDK for C++**.
- `test_login.py`: Python script using the Microsoft Authentication Library (`msal`) to perform Device Code Flow.
- `test_odbc.py`: Python script that uses `test_login.py` to acquire a token and connects to HANA via `pyodbc`.
- `SAP_HANA_ODBC_SETUP.md`: A detailed 7-step guide covering Entra ID configuration, SAP HANA certificate trust, JWT Provider setup, and user mapping.
- `.env`: Configuration file for environment-specific variables (Tenant ID, Client ID, DSN name).

## Configuration

Create a `.env` file in the root directory with the following content:

```env
ENTRA_TENANT_ID=<your-tenant-id>
ENTRA_CLIENT_ID=<your-client-id>
HANA_DSN_NAME=<your-odbc-dsn-name>
```

## Python Setup & Instructions

### 1. Install Dependencies
Ensure you have `pip3` installed, then run:
```bash
pip3 install msal pyodbc
```

### 2. Run the Sample
You can test the login process or the full ODBC connection:
```bash
# Test only the Entra ID login
python3 test_login.py

# Test the full ODBC connection to HANA
python3 test_odbc.py
```

## C++ Setup & Instructions

### 1. Install Dependencies
You will need the ODBC development headers, `libcurl`, and the `nlohmann-json` library.
On Ubuntu/Debian:
```bash
sudo apt-get install unixodbc-dev libcurl4-openssl-dev nlohmann-json3-dev
```

### 2. Compile

**Standard Compilation (Manual Bridge):**
This version uses `libcurl` to handle the OAuth2 flow manually.
```bash
g++ -o hana_sample hana_odbc_sample.cpp -lodbc -lcurl
```

**Advanced Compilation (Azure Identity SDK):**
If you have the [Azure SDK for C++](https://github.com/Azure/azure-sdk-for-cpp) installed:
```bash
g++ -o hana_sample hana_odbc_sample.cpp \
    -lodbc -lcurl -lazure-identity -lazure-core \
    -DUSE_AZURE_IDENTITY
```

### 3. Run
```bash
./hana_sample
```

## Documentation
For the full setup guide on how to configure SAP HANA Cloud and Entra ID, refer to [SAP_HANA_ODBC_SETUP.md](./SAP_HANA_ODBC_SETUP.md).
