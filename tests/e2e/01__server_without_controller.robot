*** Settings ***
Documentation     Server alone: authentication, and the seed devices waiting for their certificates.
...               A device exists only once its certificate does, and only the controller's
...               device processes can produce the CSRs.
Resource          resources/api.resource


*** Test Cases ***
Health is public
    HTTP Status Should Be    200    GET    /api/health

Devices without token is 401
    HTTP Status Should Be    401    GET    /api/devices/dummy1

Garbage token is 401
    &{headers}=    Create Dictionary    Authorization=Bearer x.y.z
    HTTP Status Should Be    401    GET    /api/devices/dummy1    headers=${headers}

Wrong password is 401
    &{body}=    Create Dictionary    username=admin    password=wrong-password
    HTTP Status Should Be    401    POST    /api/auth/login    json=${body}

Unknown user is 401
    &{body}=    Create Dictionary    username=nobody    password=wrong-password
    HTTP Status Should Be    401    POST    /api/auth/login    json=${body}

Admin login
    Login As    admin    ${ADMIN_PASSWORD}

Seeds are PENDING enrollments without the controller
    ${rows}=    Client Json    devices    enrollments
    ${pending}=    Evaluate
    ...    sorted(r['device'] for r in $rows if r['kind'] == 'ENROLL' and r['status'] == 'PENDING')
    Should Be Equal    ${pending}    ${{ ['dummy1', 'dummy2', 'dummy3'] }}

No device exists before its certificate
    ${devices}=    Client Json    devices
    Should Be Empty    ${devices}
    ${headers}=    Auth Headers
    HTTP Status Should Be    404    GET    /api/devices/dummy1    headers=${headers}

Config for a device still enrolling is 404
    ${headers}=    Auth Headers    content_type=application/json
    HTTP Status Should Be    404    POST    /api/config    headers=${headers}
    ...    data={"device":"dummy1","rate_ms":300}

Enrollment without the controller stays in progress (202)
    ${headers}=    Auth Headers
    &{body}=    Create Dictionary    name=early-1    type=sine
    ${response}=    HTTP Status Should Be    202    POST    /api/devices    headers=${headers}    json=${body}
    Should Be Equal    ${response.json()}[status]    PENDING
    Set Global Variable    ${EARLY_ID}    ${response.json()}[request_id]

The device CA is published
    ${ca}=    Client Json    pki    ca
    Length Should Be    ${ca}[roots]    1
    Should Be Equal    ${ca}[roots][0][fingerprint]    ${ca}[signing_fingerprint]
    Should Be Empty    ${ca}[revoked]
