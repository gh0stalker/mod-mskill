/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

// From the module sources
void AddMskillScripts();

// Entry point called by the AzerothCore module system.
// The function name must be Add<ModuleFolderName>Scripts with every '-'
// in the module folder name ("mod-mskill") replaced by '_'.
// cf. https://github.com/azerothcore/azerothcore-wotlk/blob/master/doc/changelog/master.md#how-to-upgrade-4
void Addmod_mskillScripts()
{
    AddMskillScripts();
}
