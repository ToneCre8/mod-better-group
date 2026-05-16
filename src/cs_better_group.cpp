/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "better_group.h"


using namespace Acore::ChatCommands;

class bettergroup_commandscript : public CommandScript
{
public:
    bettergroup_commandscript() : CommandScript("bettergroup_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable bettergroupCommandTable = {
           { "better", HandleBetterGroupCommand, SEC_GAMEMASTER, Console::No },
        };

        static ChatCommandTable commandTable = {
            { "group", bettergroupCommandTable },
        };

        return commandTable;
    }

    static bool HandleBetterGroupCommand(ChatHandler* handler, char const* args)
    {
        return BetterGroup::HandleBetterGroupCommand(handler, args);
    }

};

void AddSC_bettergroup_commandscript() { new bettergroup_commandscript(); }
