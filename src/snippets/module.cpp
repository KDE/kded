//! [module]
#include <KDEDModule>
#include <KPluginFactory>

class MyModule : public KDEDModule
{
    Q_OBJECT
public:
    MyModule(QObject *parent)
        : KDEDModule(parent)
    {
    }

    // code goes here
};

K_PLUGIN_CLASS_WITH_JSON(MyModule, "module.json")

//! [module]
