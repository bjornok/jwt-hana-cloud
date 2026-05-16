import test_login
import pyodbc

def main():
    # 1. Get Entra ID Token using the existing script
    print("Getting token from Entra ID...")
    token_result = test_login.get_entra_token()
    token = token_result.get("access_token")
    
    if not token:
        print("Failed to get access token from Entra ID.")
        return

    # Extract user claim to print who we are logging in as
    claims = token_result.get("id_token_claims", {}) or token_result.get("claims", {})
    user_email = claims.get("email") or claims.get("preferred_username") or claims.get("upn")
    print(f"Authenticated Entra ID user: {user_email}")

    # 2. Build the SAP HANA ODBC Connection String
    env_config = test_login.load_env()
    dsn_name = env_config.get("HANA_DSN_NAME")
    if not dsn_name:
        raise ValueError("Missing HANA_DSN_NAME in .env file")

    conn_str = (
        f"DSN={dsn_name};"
        "authenticationMethods=JWT;"
        f"PWD={token};"
        "ENCRYPT=TRUE;"
        "sslValidateCertificate=TRUE;"
    )

    print(f"\nConnection String:\n{conn_str}")
    print("\nConnecting to SAP HANA Cloud...")
    try:
        # 3. Connect and execute query
        conn = pyodbc.connect(conn_str)
        print(">>> SUCCESS! Connected to SAP HANA Cloud.")
        
        cursor = conn.cursor()
        cursor.execute("SELECT CURRENT_USER FROM DUMMY")
        row = cursor.fetchone()
        if row:
            print(f"HANA Current User: {row[0]}")
            
        conn.close()
    except pyodbc.Error as e:
        print(">>> FAILED to connect (ODBC Error):")
        print(e)
    except Exception as e:
        print(">>> FAILED:", e)

if __name__ == "__main__":
    main()
