idf_component_register(
    SRCS
        "phy_rpc_overlay.c"
        "phy_rpc_handlers.c"
        "phy_rpc_extras.c"
        "phy_rpc_wireless.c"
    INCLUDE_DIRS
        "."
        # The shared wire protocol lives in the parent repo so host and
        # slave share one source of truth for msg_ids and packed structs.
        "${CMAKE_CURRENT_LIST_DIR}/../../../esp_hosted_open/include"
    REQUIRES esp_wifi esp_event nvs_flash main
)

# Same undocumented PHY symbols we use in the standalone C5 build.
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,-u,phy_11p_set"
    "-Wl,-u,phy_change_channel"
)
