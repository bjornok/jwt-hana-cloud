#include <iostream>
#include <vector>
#include <string>
#include <sql.h>
#include <sqlext.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>
#include <algorithm>
#include <fstream>
#include <map>

#ifdef USE_AZURE_IDENTITY
#include <azure/identity/device_code_credential.hpp>
#include <azure/core/credentials/credentials.hpp>
#endif

std::map<std::string, std::string> loadEnv(const std::string& path = ".env") {
    std::map<std::string, std::string> env;
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        size_t pos = line.find('=');
        if (pos != std::string::npos && line[0] != '#') {
            env[line.substr(0, pos)] = line.substr(pos + 1);
        }
    }
    return env;
}

#ifdef USE_AZURE_IDENTITY
/**
 * Performs Entra ID Device Code Flow using the Microsoft Azure Identity SDK for C++.
 * This is the modern replacement for MSAL on Linux.
 */
std::string getAzureIdentityToken() {
    auto env = loadEnv();
    if (env.find("ENTRA_TENANT_ID") == env.end() || env.find("ENTRA_CLIENT_ID") == env.end()) {
        throw std::runtime_error("Missing ENTRA_TENANT_ID or ENTRA_CLIENT_ID in .env file");
    }
    std::string tenantId = env["ENTRA_TENANT_ID"];
    std::string clientId = env["ENTRA_CLIENT_ID"];
    
    // Configure Device Code options
    Azure::Identity::DeviceCodeCredentialOptions options;
    options.TenantId = tenantId;
    options.UserPromptCallback = [](Azure::Identity::DeviceCodeInfo info, Azure::Core::Context const&) {
        std::cout << "\n" << info.Message << std::endl;
    };

    auto credential = std::make_shared<Azure::Identity::DeviceCodeCredential>(clientId, options);
    
    // Request a token for the custom HANA scope
    Azure::Core::Credentials::TokenRequestContext requestContext;
    requestContext.Scopes = { "api://" + clientId + "/access_as_user" };

    auto token = credential->GetToken(requestContext);
    return token.Token;
}
#endif

using json = nlohmann::json;

/**
 * RAII Wrapper for ODBC Handles.
 */
template <SQLSMALLINT HandleType>
class SQLHandle {
public:
    SQLHandle(SQLHANDLE parent = SQL_NULL_HANDLE) : handle(SQL_NULL_HANDLE) {
        if (SQLAllocHandle(HandleType, parent, &handle) != SQL_SUCCESS) {
            handle = SQL_NULL_HANDLE;
        }
    }
    ~SQLHandle() { if (handle != SQL_NULL_HANDLE) SQLFreeHandle(HandleType, handle); }
    SQLHandle(const SQLHandle&) = delete;
    operator SQLHANDLE() const { return handle; }
    SQLHANDLE* operator&() { return &handle; }
    bool isValid() const { return handle != SQL_NULL_HANDLE; }
private:
    SQLHANDLE handle;
};

// --- Helper for libcurl response ---
size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// --- Simple Base64 Decode for JWT Payload ---
std::string base64_decode(std::string in) {
    std::string out;
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) T["ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[i]] = i;
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (T[c] == -1) continue;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

void printJwtClaims(const std::string& token) {
    try {
        size_t firstDot = token.find('.');
        size_t secondDot = token.find('.', firstDot + 1);
        if (firstDot == std::string::npos || secondDot == std::string::npos) return;

        std::string payloadBase64 = token.substr(firstDot + 1, secondDot - firstDot - 1);
        // Fix padding
        while (payloadBase64.length() % 4 != 0) payloadBase64 += '=';
        
        std::string decoded = base64_decode(payloadBase64);
        auto j = json::parse(decoded);
        
        std::cout << "\n--- JWT CLAIMS INSPECTION ---" << std::endl;
        std::cout << j.dump(4) << std::endl;
        std::cout << "-----------------------------\n" << std::endl;
    } catch (...) {
        std::cerr << "Failed to decode JWT claims for inspection." << std::endl;
    }
}

/**
 * Performs Entra ID Device Code Flow.
 */
std::string getEntraToken() {
    auto env = loadEnv();
    if (env.find("ENTRA_TENANT_ID") == env.end() || env.find("ENTRA_CLIENT_ID") == env.end()) {
        throw std::runtime_error("Missing ENTRA_TENANT_ID or ENTRA_CLIENT_ID in .env file");
    }
    const std::string tenantId = env["ENTRA_TENANT_ID"];
    const std::string clientId = env["ENTRA_CLIENT_ID"];
    const std::string scope = "api://" + clientId + "/access_as_user openid profile";
    
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("CURL init failed");

    std::string response;
    std::string deviceCodeUrl = "https://login.microsoftonline.com/" + tenantId + "/oauth2/v2.0/devicecode";
    std::string postFields = "client_id=" + clientId + "&scope=" + scope;

    curl_easy_setopt(curl, CURLOPT_URL, deviceCodeUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postFields.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    
    if (curl_easy_perform(curl) != CURLE_OK) throw std::runtime_error("Device code request failed");

    auto j = json::parse(response);
    std::cout << "\n" << j["message"].get<std::string>() << std::endl;

    std::string deviceCode = j["device_code"];
    int interval = j.value("interval", 5);

    std::string tokenUrl = "https://login.microsoftonline.com/" + tenantId + "/oauth2/v2.0/token";
    postFields = "grant_type=urn:ietf:params:oauth:grant-type:device_code&client_id=" + clientId + "&device_code=" + deviceCode;

    while (true) {
        response.clear();
        curl_easy_setopt(curl, CURLOPT_URL, tokenUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postFields.c_str());
        
        if (curl_easy_perform(curl) != CURLE_OK) throw std::runtime_error("Token polling failed");

        auto t = json::parse(response);
        if (t.contains("access_token")) {
            curl_easy_cleanup(curl);
            return t["access_token"];
        } else if (t.contains("error")) {
            std::string err = t["error"];
            if (err == "authorization_pending") {
                std::this_thread::sleep_for(std::chrono::seconds(interval));
            } else {
                throw std::runtime_error("Auth failed: " + err);
            }
        }
    }
}

int main() {
    curl_global_init(CURL_GLOBAL_ALL);
    
    try {
        std::cout << "SAP HANA Cloud Login:" << std::endl;
        std::cout << "1) Microsoft Entra ID (Manual JSON/CURL Bridge)" << std::endl;
        std::cout << "2) Basic Auth (Username/Password)" << std::endl;
        std::cout << "3) Microsoft Entra ID (Azure Identity SDK for C++)" << std::endl;
        std::cout << "Choice: ";
        int choice;
        std::cin >> choice;
        std::cin.ignore();

        auto env_config = loadEnv();
        if (env_config.find("HANA_DSN_NAME") == env_config.end()) {
            throw std::runtime_error("Missing HANA_DSN_NAME in .env file");
        }
        std::string dsnName = env_config["HANA_DSN_NAME"];
        std::string connStr = "DSN=" + dsnName + ";";

        if (choice == 1) {
            std::string jwtToken = getEntraToken();
            printJwtClaims(jwtToken);
            // The native SAP HANA ODBC driver explicitly requires authenticationMethods=JWT (plural) and the token in PWD
            connStr += "authenticationMethods=JWT;PWD=" + jwtToken + ";ENCRYPT=TRUE;sslValidateCertificate=TRUE;";
        } else if (choice == 2) {
            std::string user, pass;
            std::cout << "Username (Note: HANA users are often UPPERCASE): "; std::getline(std::cin, user);
            std::cout << "Password: "; std::getline(std::cin, pass);
            // Setting sslValidateCertificate=FALSE temporarily to rule out certificate trust issues.
            // In production, this should be TRUE with a proper sslTrustStore.
            connStr += "UID=" + user + ";PWD=" + pass + ";ENCRYPT=TRUE;sslValidateCertificate=FALSE;";
        } else if (choice == 3) {
#ifdef USE_AZURE_IDENTITY
            std::string jwtToken = getAzureIdentityToken();
            printJwtClaims(jwtToken);
            connStr += "authenticationMethods=JWT;PWD=" + jwtToken + ";ENCRYPT=TRUE;sslValidateCertificate=TRUE;";
#else
            std::cerr << "\n[ERROR] Azure Identity SDK support not compiled in.\n"
                      << "To use this option, you must install the Azure SDK for C++ and recompile with:\n"
                      << "g++ -o hana_sample hana_odbc_sample.cpp -lodbc -lcurl -lazure-identity -lazure-core -DUSE_AZURE_IDENTITY\n" << std::endl;
            return 1;
#endif
        } else {
            return 0;
        }

        // ODBC Connection
        SQLHandle<SQL_HANDLE_ENV> env;
        SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);
        SQLHandle<SQL_HANDLE_DBC> dbc(env);
        
        std::cout << "\nConnection String:\n" << connStr << "\n\n";
        std::cout << "Connecting to SAP HANA Cloud..." << std::endl;
        SQLCHAR outConnStr[1024];
        SQLSMALLINT outConnStrLen;
        SQLRETURN ret = SQLDriverConnect(dbc, NULL, (SQLCHAR*)connStr.c_str(), SQL_NTS, 
                                         outConnStr, sizeof(outConnStr), &outConnStrLen, SQL_DRIVER_NOPROMPT);
        
        if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
            SQLCHAR sqlState[6], message[1024];
            SQLINTEGER nativeError;
            SQLSMALLINT textLength;
            SQLGetDiagRec(SQL_HANDLE_DBC, dbc, 1, sqlState, &nativeError, message, sizeof(message), &textLength);
            std::cerr << "Connection failed: [" << sqlState << "] " << message << std::endl;
            return 1;
        }

        std::cout << "Connected! Executing query..." << std::endl;
        SQLHandle<SQL_HANDLE_STMT> stmt(dbc);
        SQLExecDirect(stmt, (SQLCHAR*)"SELECT CURRENT_USER FROM DUMMY", SQL_NTS);

        SQLCHAR currentUser[256];
        while (SQLFetch(stmt) == SQL_SUCCESS) {
            SQLGetData(stmt, 1, SQL_C_CHAR, currentUser, sizeof(currentUser), NULL);
            std::cout << "HANA Current User: " << currentUser << std::endl;
        }

        SQLDisconnect(dbc);
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
    }

    curl_global_cleanup();
    return 0;
}
