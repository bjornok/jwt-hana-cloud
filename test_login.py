import msal
import os

def load_env():
    env = {}
    try:
        with open('.env', 'r') as f:
            for line in f:
                line = line.strip()
                if '=' in line and not line.startswith('#'):
                    k, v = line.split('=', 1)
                    env[k] = v
    except FileNotFoundError:
        pass
    return env

env_config = load_env()

# === Entra ID Configuration ===
TENANT_ID = env_config.get("ENTRA_TENANT_ID")
CLIENT_ID = env_config.get("ENTRA_CLIENT_ID")

if not TENANT_ID or not CLIENT_ID:
    raise ValueError("Missing ENTRA_TENANT_ID or ENTRA_CLIENT_ID in .env file")

AUTHORITY = f"https://login.microsoftonline.com/{TENANT_ID}"
SCOPE = [f"api://{CLIENT_ID}/access_as_user"]



def get_entra_token():
    """Device Code Flow - easy for sample (user gets a code to enter in browser)"""
    app = msal.PublicClientApplication(CLIENT_ID, authority=AUTHORITY)
    flow = app.initiate_device_flow(scopes=SCOPE)
    print(flow["message"])  # User sees code and URL
    result = app.acquire_token_by_device_flow(flow)
    if "access_token" in result:
        # You can also use result.get("id_token") - often better for user claims
        return result
    else:
        raise Exception("Authentication failed")



# === Main Flow ===
if __name__ == "__main__":
    token_result = get_entra_token()
    
    # Extract claim (email or preferred_username / upn)
    claims = token_result.get("id_token_claims", {}) or token_result.get("claims", {})
    user_email = claims.get("email") or claims.get("preferred_username") or claims.get("upn")
    
    print(f"Authenticated Entra ID user: {user_email}")

    # Print the JWT Header to find the 'kid'
    if "access_token" in token_result:
        token = token_result["access_token"]
        import base64
        import json
        header_b64 = token.split('.')[0]
        header_b64 += "=" * ((4 - len(header_b64) % 4) % 4)
        try:
            header_json = json.loads(base64.b64decode(header_b64).decode('utf-8'))
            print(f"Token Header kid (Key ID): {header_json.get('kid')}")
        except Exception as e:
            print("Could not decode header")
    
    