#include "gtest/test_global.h"

#include "proofcore/settings.h"
#include "proofcore/settingsgroup.h"

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QCoreApplication>
#include <QSignalSpy>

using namespace Proof;

using testing::Test;

class SettingsTest: public Test
{
public:
    SettingsTest()
    {
    }

protected:
    void SetUp() override
    {
        qApp->setApplicationName(QString("proofcore_tests_%1").arg(counter++));
        QFile::remove(Settings::filePath());
    }

    void TearDown() override
    {
        QFile::remove(Settings::filePath());
    }

protected:
    void prepareSettingsFile()
    {
        QFile input(":/data/settings_read_test.conf");
        if (!input.open(QIODevice::ReadOnly))
            return;
        QFileInfo settingsFile(Settings::filePath());
        settingsFile.absoluteDir().mkpath(".");
        QFile output(Settings::filePath());
        if (!output.open(QIODevice::WriteOnly|QIODevice::Truncate|QIODevice::Text))
            return;
        QByteArray data = input.readAll();
        output.write(data);
    }

    QString appName;
    static int counter;
};

int SettingsTest::counter = 0;

TEST_F(SettingsTest, read)
{
    prepareSettingsFile();

    Settings settings;
    SettingsGroup *mainGroup = settings.mainGroup();
    SettingsGroup *firstGroup = settings.group("first_group");
    SettingsGroup *secondGroup = settings.group("second_group");
    SettingsGroup *nestedGroup = settings.group("nested");

    ASSERT_NE(nullptr, mainGroup);
    ASSERT_NE(nullptr, firstGroup);
    ASSERT_NE(nullptr, secondGroup);
    ASSERT_NE(nullptr, nestedGroup);

    SettingsGroup *nestedNestedGroup = nestedGroup->group("nested");
    SettingsGroup *nestedAnotherGroup = nestedGroup->group("another");
    ASSERT_NE(nullptr, nestedNestedGroup);
    ASSERT_NE(nullptr, nestedAnotherGroup);
    SettingsGroup *nestedNestedMoreNestedGroup = nestedNestedGroup->group("more_nested");
    ASSERT_NE(nullptr, nestedNestedMoreNestedGroup);
    SettingsGroup *nestedNestedMoreNestedOneMoreLevelGroup = nestedNestedMoreNestedGroup->group("one_more_level");
    ASSERT_NE(nullptr, nestedNestedMoreNestedOneMoreLevelGroup);

    EXPECT_EQ(3, mainGroup->groups().count());
    EXPECT_EQ(0, firstGroup->groups().count());
    EXPECT_EQ(0, secondGroup->groups().count());

    EXPECT_EQ(1, mainGroup->values().count());
    EXPECT_EQ(2, firstGroup->values().count());
    EXPECT_EQ(2, secondGroup->values().count());

    EXPECT_EQ(2, nestedGroup->groups().count());
    EXPECT_EQ(0, nestedAnotherGroup->groups().count());
    EXPECT_EQ(1, nestedNestedGroup->groups().count());
    EXPECT_EQ(1, nestedNestedMoreNestedGroup->groups().count());
    EXPECT_EQ(0, nestedNestedMoreNestedOneMoreLevelGroup->groups().count());

    EXPECT_EQ(1, nestedGroup->values().count());
    EXPECT_EQ(1, nestedNestedGroup->values().count());
    EXPECT_EQ(1, nestedAnotherGroup->values().count());
    EXPECT_EQ(1, nestedNestedMoreNestedGroup->values().count());
    EXPECT_EQ(1, nestedNestedMoreNestedOneMoreLevelGroup->values().count());

    EXPECT_EQ(42, mainGroup->value("main_group_attribute").toInt());
    EXPECT_EQ("abc", firstGroup->value("first_group_attribute").toString());
    EXPECT_TRUE(firstGroup->value("first_group_another_attribute").toBool());
    EXPECT_DOUBLE_EQ(10.5, secondGroup->value("second_group_attribute").toDouble());
    EXPECT_EQ("", secondGroup->value("second_group_another_attribute", 42).toString());

    EXPECT_EQ(QVariant(), mainGroup->value("non_existent_attribute"));
    EXPECT_EQ(QVariant(42), mainGroup->value("non_existent_attribute", 42));

    EXPECT_EQ(123, nestedNestedGroup->value("param").toInt());
    EXPECT_EQ(321, nestedAnotherGroup->value("param").toInt());
    EXPECT_EQ(456, nestedNestedMoreNestedGroup->value("param").toInt());
    EXPECT_EQ(654, nestedNestedMoreNestedOneMoreLevelGroup->value("param").toInt());
    EXPECT_EQ(987, nestedGroup->value("param").toInt());
}

TEST_F(SettingsTest, valueNotFoundPolicy)
{
    prepareSettingsFile();

    Settings settings;
    SettingsGroup *mainGroup = settings.mainGroup();

    EXPECT_EQ(42, mainGroup->value("non_existent_attribute", 42, Settings::NotFoundPolicy::DoNothing).toInt());
    EXPECT_EQ(142, mainGroup->value("non_existent_attribute", 142, Settings::NotFoundPolicy::DoNothing).toInt());

    EXPECT_EQ(42, mainGroup->value("non_existent_attribute", 42, Settings::NotFoundPolicy::Add).toInt());
    EXPECT_EQ(42, mainGroup->value("non_existent_attribute", 142, Settings::NotFoundPolicy::DoNothing).toInt());
}

TEST_F(SettingsTest, write)
{
    Settings settings;
    SettingsGroup *mainGroup = settings.mainGroup();

    QSignalSpy *mainGroupValueChangedSpy = new QSignalSpy(mainGroup, SIGNAL(valueChanged(QStringList,QVariant)));
    QSignalSpy *mainGroupGroupAddedSpy = new QSignalSpy(mainGroup, SIGNAL(groupAdded(QString)));

    mainGroup->setValue("main_first_attribute", true);
    EXPECT_EQ(1, mainGroupValueChangedSpy->count());

    mainGroup->setValue("main_second_attribute", 42);
    EXPECT_EQ(2, mainGroupValueChangedSpy->count());

    SettingsGroup *group = settings.addGroup("another");
    EXPECT_EQ(1, mainGroupGroupAddedSpy->count());

    QSignalSpy *anotherGroupValueChangedSpy = new QSignalSpy(group, SIGNAL(valueChanged(QStringList,QVariant)));

    group->setValue("another_first_attribute", "abc");
    EXPECT_EQ(1, anotherGroupValueChangedSpy->count());
    EXPECT_EQ(3, mainGroupValueChangedSpy->count());

    EXPECT_EQ(2, mainGroup->values().count());
    EXPECT_EQ(1, mainGroup->groups().count());
    EXPECT_EQ(1, group->values().count());

    EXPECT_EQ(true, mainGroup->value("main_first_attribute").toBool());
    EXPECT_EQ(42, mainGroup->value("main_second_attribute").toInt());
    EXPECT_EQ("abc", group->value("another_first_attribute").toString());

    settings.sync();

    QStringList referenceList {
        "[another]",
        "[general]",
        "another_first_attribute=abc",
        "main_first_attribute=true",
        "main_second_attribute=42"
    };

    QFile settingsFile(Settings::filePath());
    settingsFile.open(QIODevice::ReadOnly|QIODevice::Text);

    QString fromFile = QString(settingsFile.readAll()).toLower();
    fromFile.remove(" ");
    QStringList listFromFile = fromFile.split("\n", QString::SkipEmptyParts);
    listFromFile.sort();

    ASSERT_EQ(referenceList.count(), listFromFile.count());
    for (int i = 0; i < referenceList.count(); ++i)
        EXPECT_EQ(referenceList[i], listFromFile[i]);

    delete mainGroupValueChangedSpy;
    delete mainGroupGroupAddedSpy;
    delete anotherGroupValueChangedSpy;
}
