*** Settings ***
Documentation     SIGTERM reaches both processes, and each listener thread exits cleanly.
Resource          resources/stack.resource
Suite Setup       Stop And Collect Logs


*** Variables ***
${LOGS}    ${OUTPUT DIR}${/}logs


*** Test Cases ***
Server logged a clean listener exit
    ${log}=    Get File    ${LOGS}${/}server.log
    Should Contain    ${log}    listener exiting

Controller logged a clean listener exit
    ${log}=    Get File    ${LOGS}${/}controller.log
    Should Contain    ${log}    listener exiting


*** Keywords ***
Stop And Collect Logs
    Stop Services    server    controller
    Copy Service Logs    ${LOGS}
