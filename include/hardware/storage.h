#pragma once

class StorageManager
{
public:
    static StorageManager &instance();

    bool mount();
    void unmount();
    bool isMounted() const;
    const char *lastError() const;

private:
    StorageManager();

    bool mounted_;
    const char *lastError_;
};
