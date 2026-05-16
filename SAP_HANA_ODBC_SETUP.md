# SAP HANA Cloud & Microsoft Entra ID JWT Integration (Email-Based)

This guide provides the final 7-step plan to connect to SAP HANA Cloud using JWT tokens, mapped by user email addresses for scalability.

## Tenant & Client Details
- **Tenant ID:** `<TENANT_ID>`
- **Client ID:** `<CLIENT_ID>`
- **HANA Endpoint:** `<HANA_ENDPOINT>`

---

## Step 1: Entra ID App Configuration
1.  **Expose an API:** Set Application ID URI to `api://<CLIENT_ID>`. Add scope `access_as_user`.
2.  **Manifest:** Ensure `"requestedAccessTokenVersion": 2` in the `api` section.
3.  **Authentication:** Under **Advanced settings**, set **Allow public client flows** to **Yes** (required for the `password` grant/ROPC).

## Step 2: Establish Certificate Trust & Configure JWT Provider in SAP HANA
SAP HANA needs Entra ID's public keys (certificates) to verify the token signature.

1.  **Identify Required Key ID (`kid`):** Entra ID uses multiple active keys. You must import the certificate that matches the `kid` in your token's header. You can extract it using a JWT decoder (like jwt.io) or by running this Python snippet on a generated token:
    ```python
    import json, base64
    def get_kid(token):
        header_b64 = token.split('.')[0]
        header_b64 += "=" * ((4 - len(header_b64) % 4) % 4)
        print("Required kid:", json.loads(base64.b64decode(header_b64).decode('utf-8'))['kid'])
    ```
2.  **Get Public Keys:** Fetch the active keys from Entra ID's JWKS endpoint:
    `curl -s https://login.microsoftonline.com/<TENANT_ID>/discovery/v2.0/keys`
3.  **Format Certificate:** Locate the key array element where the `kid` matches your token. Copy its `x5c` value and wrap it in standard PEM format:
    ```text
    -----BEGIN CERTIFICATE-----
    <x5c_base64_string>
    -----END CERTIFICATE-----
    ```
4.  **Execute in HANA SQL Console:** Run the following to create the certificate, PSE, and JWT Provider.
    ```sql
    -- 1. Create the Certificate object
    CREATE CERTIFICATE ENTRA_ID_CERT_1 FROM '
    -----BEGIN CERTIFICATE-----
    MIIC... (your full certificate string) ...=
    -----END CERTIFICATE-----
    ';

    -- 2. Create a PSE (Personal Security Environment)
    CREATE PSE ENTRA_ID_PSE;

    -- 3. Add the certificate to the PSE
    ALTER PSE ENTRA_ID_PSE ADD CERTIFICATE ENTRA_ID_CERT_1;

    -- 4. Create the JWT Provider
    CREATE JWT PROVIDER ENTRA_ID_PROVIDER 
      WITH ISSUER 'https://login.microsoftonline.com/<TENANT_ID>/v2.0' 
      CLAIM 'preferred_username' AS EXTERNAL IDENTITY;

    -- 5. (Optional) Add the Audience (the Entra ID Client ID) to the Provider
    -- If your HANA setup requires explicit audience validation, use this command:
    -- ALTER JWT PROVIDER ENTRA_ID_PROVIDER ADD CLAIM 'aud' = '<CLIENT_ID>';

    -- 6. Link the PSE to the JWT Provider
    SET PSE ENTRA_ID_PSE PURPOSE JWT FOR PROVIDER ENTRA_ID_PROVIDER;
    ```
    *Note: Microsoft periodically rotates these keys. When authentication fails with Error 10 in the future, check if the `kid` has changed and import the new certificate.*

## Step 3: Map Database Users
Map users by their actual Entra ID email addresses.
```sql
CREATE USER HANA_USER_DEMO PASSWORD "SomeStrongPassword123!" NO FORCE_FIRST_PASSWORD_CHANGE;

ALTER USER HANA_USER_DEMO ADD IDENTITY 'user@yourdomain.com' FOR JWT PROVIDER ENTRA_ID_PROVIDER;
ALTER USER HANA_USER_DEMO ENABLE JWT;

GRANT RESTRICTED_USER_JDBC_ACCESS TO HANA_USER_DEMO;
GRANT PUBLIC TO HANA_USER_DEMO;
```

## Step 4: Configure Local ODBC (DSN)
Ensure `/etc/odbc.ini` contains:
```ini
[HANA_DSN]
Driver=HDBODBC
ServerNode=<HANA_ENDPOINT>
```

## Step 5: Obtain JWT Token (User Context)
Use the `password` grant to get a token for a specific user.
```bash
curl -X POST -H "Content-Type: application/x-www-form-urlencoded" \
"https://login.microsoftonline.com/<TENANT_ID>/oauth2/v2.0/token" \
-d "client_id=<CLIENT_ID>" \
-d "scope=api://<CLIENT_ID>/access_as_user openid profile" \
-d "client_secret=<CLIENT_SECRET>" \
-d "grant_type=password" \
-d "username=user@yourdomain.com" \
-d "password=<USER_PASSWORD>"
```
Copy the `access_token` from the response.

## Step 6: Compile & Run C++ Sample
The C++ sample now supports three login methods:
1.  **Manual JSON/CURL Bridge:** Uses standard libraries (libcurl, nlohmann-json) to manually perform the Device Code Flow.
2.  **Basic Auth:** Standard Username/Password.
3.  **Azure Identity SDK for C++:** Uses Microsoft's official SDK (recommended for production).

### Standard Compilation (Manual Bridge)
```bash
g++ -o hana_sample hana_odbc_sample.cpp -lodbc -lcurl
```

### Advanced Compilation (Azure Identity SDK)
If you have the [Azure SDK for C++](https://github.com/Azure/azure-sdk-for-cpp) installed, you can compile with the official libraries:
```bash
g++ -o hana_sample hana_odbc_sample.cpp \
    -lodbc -lcurl -lazure-identity -lazure-core \
    -DUSE_AZURE_IDENTITY
```

## Step 7: Execution & Validation
1.  Run `./hana_sample`.
2.  Choose your preferred authentication method.
3.  Success: `HANA Current User: DEVUSER`.

---

## Troubleshooting "Error 10"
If your connection string is correctly configured but the SAP HANA backend rejects the login, you will see an error similar to this:
```text
[HY000] [SAP AG][LIBODBCHDB SO][HDBODBC] General error;10 authentication failed: Detailed info for this error can be found with correlation ID '<CORRELATION_ID>'
```
This means the client sent the token successfully, but SAP HANA rejected it (e.g., Issuer mismatch, invalid signature, or user mapping issue).

**To find the exact reason, run this query in the SAP HANA SQL Console:**
```sql
SELECT * FROM SYS.AUTHENTICATION_ERROR_DETAILS ORDER BY TIMESTAMP DESC;
-- Alternatively, search by the Correlation ID from your error message:
-- SELECT * FROM SYS.AUTHENTICATION_ERROR_DETAILS WHERE CORRELATION_ID = '<CORRELATION_ID>';
```
Look at the `REASON` column. Common fixes:
*   `JWT provider does not exist: issuer=...`: Your configured `ISSUER` string in HANA does not exactly match the `iss` claim in the token.
*   `No matching user found`: The user identity is not mapped correctly in Step 3.
*   `Invalid signature` or `Certificate not found`: The certificate in `ENTRA_ID_PSE` is missing, formatted incorrectly (requires strict 64-char lines), or the Key ID (`kid`) changed.

---

## Troubleshooting Client-Side ODBC Errors
If the SAP HANA driver fails before even reaching the server, you will see `Communication link failure` along with specific `RTE` codes. These are caused by connection string misconfigurations.

### RTE [200117] Failed to initiate any authentication method
```text
Connection failed (RTE:[200117] Failed to initiate any authentication method. Please ensure all relevant connection properties have been supplied correctly.
```
*   **Cause:** The ODBC driver did not understand the JWT parameters and fell back to X.509/Kerberos (which also failed).
*   **Fix:** Ensure you are using `authenticationMethods=JWT` (plural) and that your SAP HANA Client is version 2.0 or higher. If the driver is too old, it will silently ignore the parameter.

### RTE [200116] No compatible authentication methods could be found
```text
Connection failed (RTE:[200116] No compatible authentication methods could be found. The connection properties required for the selected authentication method(s) are missing or invalid.
```
*   **Cause:** The driver recognized the intent to use JWT but couldn't execute it due to conflicting or missing parameters.
*   **Fix:** Ensure you are leaving the `UID` parameter explicitly empty (`UID=;`) and passing the token exactly in the `PWD` parameter (`PWD=<token>;`). Also, ensure encryption is enabled (`ENCRYPT=TRUE;`), as it is mandatory for JWT.
