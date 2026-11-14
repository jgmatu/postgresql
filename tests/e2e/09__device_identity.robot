*** Settings ***
Documentation     Device identity: a certificateHold suspends a device and can be lifted, a permanent
...               revocation takes it out of service without deleting it, a rotation leaves two
...               certificates with the old one superseded, deleting an old certificate keeps the
...               device and deleting the current one deletes it. The signed CRL lists the revoked
...               serials, and ``--max-devices`` leaves the devices beyond it UNMANAGED.
Resource          resources/api.resource
Suite Setup       Enroll The Identity Device
Suite Teardown    Destroy Stack


*** Variables ***
${DEVICE}         ident-a


*** Test Cases ***
A hold suspends the device and stops its process
    ${result}=    Client Json    devices    revoke    ${DEVICE}    certificateHold
    Should Be Equal    ${result}[identity]    SUSPENDED
    Wait Until Keyword Succeeds    15s    0.5s    Session Should Be    ${DEVICE}    DISABLED
    ${dev}=    Client Json    data    ${DEVICE}
    Should Be Equal    ${dev}[process][pid]    ${None}
    Telemetry Should Have Stopped    ${DEVICE}

Lifting the hold brings it back
    ${result}=    Client Json    devices    unrevoke    ${DEVICE}
    Should Be Equal    ${result}[identity]    ACTIVE
    Wait Until Keyword Succeeds    15s    0.5s    Session Should Be    ${DEVICE}    ONLINE
    ${seq}=    Last Seq    ${DEVICE}
    Wait Until Keyword Succeeds    10s    0.5s    Seq Should Advance    ${DEVICE}    ${seq}

A hold that is not there cannot be lifted (409)
    ${headers}=    Auth Headers
    HTTP Status Should Be    409    DELETE    /api/devices/${DEVICE}/revoke    headers=${headers}

Superseded is not a reason an admin can give (400)
    ${headers}=    Auth Headers
    &{body}=    Create Dictionary    reason=superseded
    HTTP Status Should Be    400    POST    /api/devices/${DEVICE}/revoke    headers=${headers}    json=${body}

A normal user cannot revoke (403)
    ${headers}=    Normal User Headers
    &{body}=    Create Dictionary    reason=keyCompromise
    HTTP Status Should Be    403    POST    /api/devices/${DEVICE}/revoke    headers=${headers}    json=${body}

Rotation leaves two certificates, the old one superseded
    ${before}=    Client Json    devices    certs    ${DEVICE}
    ${headers}=    Auth Headers
    ${response}=    HTTP Status Should Be    200    POST    /api/devices/${DEVICE}/rotate    headers=${headers}
    Should Be Equal    ${response.json()}[status]    ISSUED
    Should Be Equal    ${response.json()}[enrollment][kind]    ROTATE
    ${certs}=    Client Json    devices    certs    ${DEVICE}
    Length Should Be    ${certs}    2
    Should Be True    ${certs}[0][current]
    Should Not Be True    ${certs}[1][current]
    Should Be Equal    ${certs}[1][serial]    ${before}[0][serial]
    Should Be Equal    ${certs}[1][revocation_reason]    superseded
    Should Be Equal    ${certs}[0][revocation_reason]    ${None}
    Set Suite Variable    ${OLD_SERIAL}    ${certs}[1][serial]
    Set Suite Variable    ${CURRENT_SERIAL}    ${certs}[0][serial]
    Wait Until Keyword Succeeds    15s    0.5s    Session Should Be    ${DEVICE}    ONLINE

Deleting the old certificate keeps the device
    ${result}=    Client Json    devices    delcert    ${DEVICE}    ${OLD_SERIAL}
    Should Not Be True    ${result}[device_deleted]
    ${certs}=    Client Json    devices    certs    ${DEVICE}
    Length Should Be    ${certs}    1
    Identity Should Be    ${DEVICE}    ACTIVE
    Session Should Be    ${DEVICE}    ONLINE

The signed CRL still lists the deleted certificate
    CRL Should List    ${OLD_SERIAL}

A permanent revocation takes the device out of service
    ${result}=    Client Json    devices    revoke    ${DEVICE}    keyCompromise
    Should Be Equal    ${result}[identity]    REVOKED
    Wait Until Keyword Succeeds    15s    0.5s    Session Should Be    ${DEVICE}    DISABLED
    Telemetry Should Have Stopped    ${DEVICE}
    CRL Should List    ${CURRENT_SERIAL}

A revoked device is kept, with its telemetry
    ${headers}=    Auth Headers
    HTTP Status Should Be    200    GET    /api/devices/${DEVICE}    headers=${headers}
    ${rows}=    Client Json    history    ${DEVICE}    limit=1
    Length Should Be    ${rows}    1

A permanent revocation cannot be lifted or rotated away (409)
    ${headers}=    Auth Headers
    HTTP Status Should Be    409    DELETE    /api/devices/${DEVICE}/revoke    headers=${headers}
    HTTP Status Should Be    409    POST    /api/devices/${DEVICE}/rotate    headers=${headers}
    &{body}=    Create Dictionary    reason=certificateHold
    HTTP Status Should Be    409    POST    /api/devices/${DEVICE}/revoke    headers=${headers}    json=${body}

Deleting the current certificate deletes the device
    ${result}=    Client Json    devices    delcert    ${DEVICE}    ${CURRENT_SERIAL}
    Should Be True    ${result}[device_deleted]
    ${headers}=    Auth Headers
    HTTP Status Should Be    404    GET    /api/devices/${DEVICE}    headers=${headers}
    Wait Until Keyword Succeeds    15s    0.5s    Controller Path Should Not Exist    /app/run/devices/${DEVICE}
    CRL Should List    ${CURRENT_SERIAL}

Devices beyond --max-devices are UNMANAGED
    Restart Controller With Max Devices    2
    Wait Until Keyword Succeeds    30s    0.5s    Seed Sessions Should Count    ONLINE    2
    Wait Until Keyword Succeeds    15s    0.5s    Seed Sessions Should Count    UNMANAGED    1

An enrollment beyond --max-devices is refused (422 CAPACITY)
    ${headers}=    Auth Headers
    &{body}=    Create Dictionary    name=ident-b    type=sine
    ${response}=    HTTP Status Should Be    422    POST    /api/devices    headers=${headers}    json=${body}
    Should Be Equal    ${response.json()}[enrollment][error_code]    CAPACITY
    HTTP Status Should Be    404    GET    /api/devices/ident-b    headers=${headers}

Raising the limit starts every device again
    Restart Controller With Max Devices    ${None}
    Wait Until Keyword Succeeds    30s    0.5s    Seeds Should Be Online


*** Keywords ***
Enroll The Identity Device
    Arm Fleet
    Ensure Alice
    Login And Enroll

Login And Enroll
    Login As    admin    ${ADMIN_PASSWORD}
    ${result}=    Client Json    devices    add    ${DEVICE}    sine
    Should Be Equal    ${result}[status]    ISSUED
    Wait Until Keyword Succeeds    15s    0.5s    Session Should Be    ${DEVICE}    ONLINE
    Wait Until Keyword Succeeds    10s    0.5s    Seq Should Advance    ${DEVICE}    0

Telemetry Should Have Stopped
    [Arguments]    ${device}
    ${seq}=    Last Seq    ${device}
    Sleep    2.5s
    ${after}=    Last Seq    ${device}
    Should Be Equal    ${after}    ${seq}    ${device} still produces samples

CRL Should List
    [Documentation]    The CRL verifies against the published root CA and names ``serial``.
    [Arguments]    ${serial}
    ${ca}=    Client Json    pki    ca
    Create File    ${WORK}${/}device-ca.pem    ${ca}[roots][0][cert_pem]
    ${pem}=    Client Should Succeed    pki    crl
    Should Start With    ${pem}    -----BEGIN X509 CRL-----
    Create File    ${WORK}${/}devices.crl    ${pem}
    ${result}=    Run Live    openssl    crl    -in    ${WORK}${/}devices.crl
    ...    -CAfile    ${WORK}${/}device-ca.pem    -noout    -text
    Should Be Equal As Integers    ${result.rc}    0    ${result.stderr}
    Should Contain    ${result.stdout}${result.stderr}    verify OK
    ${upper}=    Convert To Upper Case    ${serial}
    Should Contain    ${result.stdout}    Serial Number: ${upper}

Seed Sessions Should Count
    [Arguments]    ${state}    ${expected}
    ${states}=    Sessions Of Seeds
    ${count}=    Evaluate    $states.count($state)
    Should Be Equal As Integers    ${count}    ${expected}    seed sessions: ${states}

Restart Controller With Max Devices
    [Documentation]    Recreate the controller with ``PGDEMO_MAX_DEVICES`` (``None``: the default).
    ...                The device keys live in the ``device-keys`` volume and survive.
    [Arguments]    ${max}
    ${content}=    Get File    ${ENV_FILE}
    ${content}=    Evaluate    ''.join(l for l in $content.splitlines(True) if not l.startswith('PGDEMO_MAX_DEVICES='))
    IF    $max is not None
        ${content}=    Set Variable    ${content}PGDEMO_MAX_DEVICES=${max}\n
    END
    Create File    ${ENV_FILE}    ${content}
    Compose    up    -d    --no-deps    --wait    controller
