idf_component_register(SRCS "pcf85063.c" "pcf85063_bcd.c"
                       INCLUDE_DIRS "include"
                       REQUIRES i2c_bus
                       PRIV_REQUIRES driver)
