ifeq ($(CONFIG_OF),y)

dtb-$(CONFIG_BOARD_GTS28WIFI) += exynos5433-gts28wifi_eur_open_05.dtb \
exynos5433-gts28wifi_eur_open_06.dtb

dtb-$(CONFIG_BOARD_GTS28LTEXX) += exynos5433-gts28_eur_open_03.dtb \
exynos5433-gts28_eur_open_04.dtb \
exynos5433-gts28_eur_open_05.dtb \
exynos5433-gts28_eur_open_06.dtb \
exynos5433-gts28_eur_open_09.dtb

dtb-$(CONFIG_BOARD_GTS210WIFI) += exynos5433-gts210wifi_eur_open_04.dtb \
exynos5433-gts210wifi_eur_open_05.dtb \
exynos5433-gts210wifi_eur_open_06.dtb

dtb-$(CONFIG_BOARD_GTS210LTEXX) += exynos5433-gts210_eur_open_03.dtb \
exynos5433-gts210_eur_open_04.dtb \
exynos5433-gts210_eur_open_05.dtb \
exynos5433-gts210_eur_open_06.dtb

dtb-$(CONFIG_BOARD_TRELTEXX) += exynos5433-tre_eur_open_07.dtb \
exynos5433-tre_eur_open_08.dtb \
exynos5433-tre_eur_open_09.dtb \
exynos5433-tre_eur_open_10.dtb \
exynos5433-tre_eur_open_12.dtb \
exynos5433-tre_eur_open_13.dtb \
exynos5433-tre_eur_open_14.dtb \
exynos5433-tre_eur_open_16.dtb 

dtb-$(CONFIG_BOARD_TRELTESKT) += exynos5433-trelte_kor_open_06.dtb\
 exynos5433-trelte_kor_open_07.dtb \
 exynos5433-trelte_kor_open_09.dtb \
 exynos5433-trelte_kor_open_11.dtb \
 exynos5433-trelte_kor_open_12.dtb

 dtb-$(CONFIG_BOARD_TRHPLTEXX) += exynos5433-trlte_eur_open_00.dtb \
exynos5433-trlte_eur_open_09.dtb \
exynos5433-trlte_eur_open_10.dtb \
exynos5433-trlte_eur_open_11.dtb \
exynos5433-trlte_eur_open_12.dtb

dtb-$(CONFIG_BOARD_TBELTESKT) += exynos5433-tbelte_kor_open_07.dtb \
exynos5433-tbelte_kor_open_09.dtb \
exynos5433-tbelte_kor_open_11.dtb \
exynos5433-tbelte_kor_open_12.dtb \
exynos5433-tbelte_kor_open_14.dtb

dtb-$(CONFIG_BOARD_TRE3CALTESKT) += exynos5433-tre3calte_kor_open_05.dtb \
exynos5433-tre3calte_kor_open_14.dtb

DTB_NAMES := $(subst $\",,$(CONFIG_BUILD_ARM_APPENDED_DTB_IMAGE_NAMES))
ifneq ($(DTB_NAMES),)
DTB_LIST := $(addsuffix .dtb,$(DTB_NAMES))
else
DTB_LIST := $(dtb-y)
endif

targets += dtbs
targets += $(DTB_LIST)
endif

# *.dtb used to be generated in the directory above. Clean out the
# old build results so people don't accidentally use them.
dtbs: $(addprefix $(obj)/, $(DTB_LIST))
	$(Q)rm -f $(obj)/../*.dtb

clean-files := *.dtb