/* targets/raspberry-pi-os/main/rpios_app_platform.cpp
 * AppManager platform stub. Single-app build — Apps.launch() just records the
 * requested id and returns success. No process swap.
 * (Linux analogue of targets/geaos/main/geaos_app_platform.cpp.) */
#include "apps.h"
#include <string>

namespace {

class LinuxAppLauncherPlatform final : public gea::framework::apps::AppLauncherPlatform {
public:
	explicit LinuxAppLauncherPlatform(std::string currentId) : currentId_(std::move(currentId)) {}
	const char *currentInstalledAppId() override { return currentId_.c_str(); }
	bool runningAppIsLauncher(const char *launcherAppId) override
	{
		if (!launcherAppId) return false;
		return currentId_ == launcherAppId;
	}
	bool launchInstalledApp(const char *) override { return false; }
private:
	std::string currentId_;
};

}  // namespace

namespace gea::rpios {

void installAppLauncherPlatform(const char *currentAppId)
{
	static LinuxAppLauncherPlatform *instance = nullptr;
	if (instance) return;
	instance = new LinuxAppLauncherPlatform(currentAppId ? currentAppId : "");
	gea::framework::apps::AppManager::setPlatform(instance);
}

}  // namespace gea::rpios
