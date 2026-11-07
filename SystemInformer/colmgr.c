/*
 * Copyright (c) 2022 Winsider Seminars & Solutions, Inc.  All rights reserved.
 *
 * This file is part of System Informer.
 *
 * Authors:
 *
 *     wj32    2011-2016
 *     dmex    2017-2026
 *
 */

#include <phapp.h>
#include <colmgr.h>

#include <extmgri.h>
#include <phplug.h>

typedef struct _PH_CM_SORT_CONTEXT
{
    PPH_PLUGIN_TREENEW_SORT_FUNCTION SortFunction;
    ULONG SubId;
    PVOID Context;
    PPH_CM_POST_SORT_FUNCTION PostSortFunction;
    PH_SORT_ORDER SortOrder;
} PH_CM_SORT_CONTEXT, *PPH_CM_SORT_CONTEXT;

typedef struct _PH_CM_SORT_KEY
{
    PPH_CM_COMPARE_FUNCTION CompareFunction; // Built-in column
    PVOID CompareContext;
    PPH_PLUGIN_TREENEW_SORT_FUNCTION PluginSortFunction; // Plugin column
    ULONG SubId;
    PVOID PluginContext;
    PH_SORT_ORDER SortOrder;
} PH_CM_SORT_KEY, *PPH_CM_SORT_KEY;

typedef struct _PH_CM_MULTI_SORT_CONTEXT
{
    ULONG NumberOfKeys;
    PH_CM_SORT_KEY Keys[PH_TREENEW_MAX_SORT_KEYS];
    PPH_CM_POST_SORT_FUNCTION PostSortFunction;
    PH_SORT_ORDER PrimarySortOrder;
} PH_CM_MULTI_SORT_CONTEXT, *PPH_CM_MULTI_SORT_CONTEXT;

/**
 * Initializes a column manager instance.
 *
 * \param Manager Receives the initialized column manager structure.
 * \param Handle A handle to the TreeNew control window.
 * \param MinId The minimum column ID reserved for plugin columns.
 * \param PostSortFunction An optional post-sort comparison callback function.
 */
VOID PhCmInitializeManager(
    _Out_ PPH_CM_MANAGER Manager,
    _In_ HWND Handle,
    _In_ ULONG MinId,
    _In_ PPH_CM_POST_SORT_FUNCTION PostSortFunction
    )
{
    Manager->Handle = Handle;
    Manager->MinId = MinId;
    Manager->NextId = MinId;
    Manager->PostSortFunction = PostSortFunction;
    Manager->SortFunctions = NULL;
    Manager->NumberOfSortFunctions = 0;
    Manager->SortContext = NULL;
    InitializeListHead(&Manager->ColumnListHead);
    Manager->NotifyList = NULL;
}

/**
 * Deletes a column manager instance and frees associated resources.
 *
 * \param Manager A pointer to the column manager to delete.
 */
VOID PhCmDeleteManager(
    _In_ PPH_CM_MANAGER Manager
    )
{
    PLIST_ENTRY listEntry;
    PPH_CM_COLUMN column;

    listEntry = Manager->ColumnListHead.Flink;

    while (listEntry != &Manager->ColumnListHead)
    {
        column = CONTAINING_RECORD(listEntry, PH_CM_COLUMN, ListEntry);
        listEntry = listEntry->Flink;

        PhFree(column);
    }

    if (Manager->NotifyList)
        PhDereferenceObject(Manager->NotifyList);
}

/**
 * Creates and registers a plugin column with the column manager and TreeNew control.
 *
 * \param Manager A pointer to the column manager.
 * \param Column A pointer to the TreeNew column definition.
 * \param Plugin A pointer to the plugin owning the column.
 * \param SubId The plugin-specific column sub-identifier.
 * \param Context An optional user-defined context pointer.
 * \param SortFunction An optional sort callback function.
 * \return PPH_CM_COLUMN A pointer to the created column structure.
 */
PPH_CM_COLUMN PhCmCreateColumn(
    _Inout_ PPH_CM_MANAGER Manager,
    _In_ PPH_TREENEW_COLUMN Column,
    _In_ PPH_PLUGIN Plugin,
    _In_ ULONG SubId,
    _In_opt_ PVOID Context,
    _In_opt_ PVOID SortFunction
    )
{
    PPH_CM_COLUMN column;
    PH_TREENEW_COLUMN tnColumn;

    column = PhAllocateZero(sizeof(PH_CM_COLUMN));
    column->Id = Manager->NextId++;
    column->Plugin = Plugin;
    column->SubId = SubId;
    column->Context = Context;
    column->SortFunction = SortFunction;
    InsertTailList(&Manager->ColumnListHead, &column->ListEntry);

    memset(&tnColumn, 0, sizeof(PH_TREENEW_COLUMN));
    tnColumn.Id = column->Id;
    tnColumn.Context = column;
    tnColumn.Visible = Column->Visible;
    tnColumn.CustomDraw = Column->CustomDraw;
    tnColumn.SortDescending = Column->SortDescending;
    tnColumn.Text = Column->Text;
    tnColumn.Width = Column->Width;
    tnColumn.Alignment = Column->Alignment;
    tnColumn.DisplayIndex = Column->Visible ? Column->DisplayIndex : ULONG_MAX;
    tnColumn.TextFlags = Column->TextFlags;
    tnColumn.HeaderTextCache = Column->HeaderTextCache;
    TreeNew_AddColumn(Manager->Handle, &tnColumn);

    return column;
}

/**
 * Finds a registered plugin column by plugin name and sub-identifier.
 *
 * \param Manager A pointer to the column manager.
 * \param PluginName The name of the owning plugin.
 * \param SubId The plugin-specific column sub-identifier.
 * \return PPH_CM_COLUMN A pointer to the column structure, or NULL if not found.
 */
PPH_CM_COLUMN PhCmFindColumn(
    _In_ PPH_CM_MANAGER Manager,
    _In_ PCPH_STRINGREF PluginName,
    _In_ ULONG SubId
    )
{
    PLIST_ENTRY listEntry;
    PPH_CM_COLUMN column;

    listEntry = Manager->ColumnListHead.Flink;

    while (listEntry != &Manager->ColumnListHead)
    {
        column = CONTAINING_RECORD(listEntry, PH_CM_COLUMN, ListEntry);

        if (column->SubId == SubId && PhEqualStringRef(PluginName, &column->Plugin->AppContext.AppName, FALSE))
            return column;

        listEntry = listEntry->Flink;
    }

    return NULL;
}

/**
 * Registers a plugin to receive notifications for all TreeNew messages.
 *
 * \param Manager A pointer to the column manager.
 * \param Plugin A pointer to the plugin to receive notifications.
 */
VOID PhCmSetNotifyPlugin(
    _In_ PPH_CM_MANAGER Manager,
    _In_ PPH_PLUGIN Plugin
    )
{
    if (!Manager->NotifyList)
    {
        Manager->NotifyList = PhCreateList(8);
    }
    else
    {
        if (PhFindItemList(Manager->NotifyList, Plugin) != ULONG_MAX)
            return;
    }

    PhAddItemList(Manager->NotifyList, Plugin);
}

/**
 * Forwards a TreeNew control message to the appropriate plugin column callback.
 *
 * \param WindowHandle A handle to the TreeNew control window.
 * \param Message The TreeNew message code.
 * \param Parameter1 The first message parameter.
 * \param Parameter2 The second message parameter.
 * \param Manager A pointer to the column manager.
 * \return BOOLEAN TRUE if the message was handled by a plugin, FALSE otherwise.
 */
BOOLEAN PhCmForwardMessage(
    _In_ HWND WindowHandle,
    _In_ PH_TREENEW_MESSAGE Message,
    _In_ PVOID Parameter1,
    _In_ PVOID Parameter2,
    _In_ PPH_CM_MANAGER Manager
    )
{
    PH_PLUGIN_TREENEW_MESSAGE pluginMessage;
    PPH_PLUGIN plugin;

    switch (Message)
    {
    case TreeNewDestroying:
        {
            return FALSE;
        }
        break;
    case TreeNewGetCellText:
        {
            PPH_TREENEW_GET_CELL_TEXT getCellText = Parameter1;
            PH_TREENEW_COLUMN tnColumn;
            PPH_CM_COLUMN column;

            if (getCellText->Id < Manager->MinId)
                return FALSE;

            if (!TreeNew_GetColumn(WindowHandle, getCellText->Id, &tnColumn))
                return FALSE;

            column = tnColumn.Context;
            pluginMessage.SubId = column->SubId;
            pluginMessage.Context = column->Context;
            plugin = column->Plugin;
        }
        break;
    case TreeNewCustomDraw:
        {
            PPH_TREENEW_CUSTOM_DRAW customDraw = Parameter1;
            PPH_CM_COLUMN column;

            if (customDraw->Column->Id < Manager->MinId)
                return FALSE;

            column = customDraw->Column->Context;
            pluginMessage.SubId = column->SubId;
            pluginMessage.Context = column->Context;
            plugin = column->Plugin;
        }
        break;
    case TreeNewColumnResized:
        {
            PPH_TREENEW_COLUMN tlColumn = Parameter1;
            PPH_CM_COLUMN column;

            if (tlColumn->Id < Manager->MinId)
                return FALSE;

            column = tlColumn->Context;
            pluginMessage.SubId = column->SubId;
            pluginMessage.Context = column->Context;
            plugin = column->Plugin;
        }
        break;
    case TreeNewGetHeaderText:
        {
            PPH_TREENEW_GET_HEADER_TEXT getHeaderText = Parameter1;
            PPH_TREENEW_COLUMN tlColumn = getHeaderText->Column;
            PPH_CM_COLUMN column;

            if (tlColumn->Id < Manager->MinId)
                return FALSE;

            column = tlColumn->Context;
            pluginMessage.SubId = column->SubId;
            pluginMessage.Context = column->Context;
            plugin = column->Plugin;
        }
        break;
    default:
        {
            // Some plugins want to be notified about all messages.
            if (Manager->NotifyList)
            {
                for (ULONG i = 0; i < Manager->NotifyList->Count; i++)
                {
                    plugin = Manager->NotifyList->Items[i];

                    pluginMessage.TreeNewHandle = WindowHandle;
                    pluginMessage.Message = Message;
                    pluginMessage.Parameter1 = Parameter1;
                    pluginMessage.Parameter2 = Parameter2;
                    pluginMessage.SubId = 0;
                    pluginMessage.Context = NULL;

                    PhInvokeCallback(PhGetPluginCallback(plugin, PluginCallbackTreeNewMessage), &pluginMessage);
                }
            }
        }
        return FALSE;
    }

    pluginMessage.TreeNewHandle = WindowHandle;
    pluginMessage.Message = Message;
    pluginMessage.Parameter1 = Parameter1;
    pluginMessage.Parameter2 = Parameter2;

    PhInvokeCallback(PhGetPluginCallback(plugin, PluginCallbackTreeNewMessage), &pluginMessage);

    return TRUE;
}

/**
 * Comparison function wrapper for sorting TreeNew nodes with plugin and post-sort callbacks.
 *
 * \param context A pointer to the PH_CM_SORT_CONTEXT structure.
 * \param elem1 A pointer to the first node element.
 * \param elem2 A pointer to the second node element.
 * \return int Negative if elem1 < elem2, zero if equal, positive if elem1 > elem2.
 */
static int __cdecl PhCmpSortFunction(
    _In_ void *context,
    _In_ const void *elem1,
    _In_ const void *elem2
    )
{
    PPH_CM_SORT_CONTEXT sortContext = context;
    PVOID node1 = *(PVOID *)elem1;
    PVOID node2 = *(PVOID *)elem2;
    LONG result;

    result = sortContext->SortFunction(node1, node2, sortContext->SubId, sortContext->SortOrder, sortContext->Context);

    return sortContext->PostSortFunction(result, node1, node2, sortContext->SortOrder);
}

/**
 * Sorts an array of TreeNew nodes using the sort function registered for a plugin column.
 *
 * \param Nodes An array of pointers to TreeNew nodes to sort.
 * \param NumberOfNodes The number of nodes in the array.
 * \param SortColumn The ID of the column being sorted.
 * \param SortOrder The sort order direction.
 * \param Manager A pointer to the column manager.
 * \return BOOLEAN TRUE if sorting was handled or bypassed, FALSE if SortColumn is not a plugin column.
 */
BOOLEAN PhCmForwardSort(
    _In_ PPH_TREENEW_NODE *Nodes,
    _In_ ULONG NumberOfNodes,
    _In_ ULONG SortColumn,
    _In_ PH_SORT_ORDER SortOrder,
    _In_ PPH_CM_MANAGER Manager
    )
{
    PH_TREENEW_COLUMN tnColumn;
    PPH_CM_COLUMN column;
    PH_CM_SORT_CONTEXT sortContext;

    if (SortColumn < Manager->MinId)
        return FALSE;

    if (!Manager->Handle)
        return TRUE;

    if (!TreeNew_GetColumn(Manager->Handle, SortColumn, &tnColumn))
        return TRUE;

    column = tnColumn.Context;

    if (!column->SortFunction)
        return TRUE;

    sortContext.SortFunction = (PPH_PLUGIN_TREENEW_SORT_FUNCTION)column->SortFunction;
    sortContext.SubId = column->SubId;
    sortContext.Context = column->Context;
    sortContext.PostSortFunction = Manager->PostSortFunction;
    sortContext.SortOrder = SortOrder;
    qsort_s(Nodes, NumberOfNodes, sizeof(PVOID), PhCmpSortFunction, &sortContext);

    return TRUE;
}

/**
 * Registers the comparison functions for the built-in (non-plugin) columns of a
 * TreeNew control so that PhCmSortNodes can sort by multiple keys.
 *
 * \param Manager A pointer to the column manager.
 * \param SortFunctions An array of comparison functions indexed by column ID. Entries may be NULL.
 * The array must remain valid for the lifetime of the manager.
 * \param NumberOfSortFunctions The number of entries in the array.
 * \param Context A context value passed to each comparison function.
 */
VOID PhCmRegisterSortFunctions(
    _Inout_ PPH_CM_MANAGER Manager,
    _In_reads_(NumberOfSortFunctions) const PPH_CM_COMPARE_FUNCTION *SortFunctions,
    _In_ ULONG NumberOfSortFunctions,
    _In_opt_ PVOID Context
    )
{
    Manager->SortFunctions = SortFunctions;
    Manager->NumberOfSortFunctions = NumberOfSortFunctions;
    Manager->SortContext = Context;
}

static int __cdecl PhCmpMultiSortFunction(
    _In_ void *context,
    _In_ const void *elem1,
    _In_ const void *elem2
    )
{
    PPH_CM_MULTI_SORT_CONTEXT sortContext = context;
    PVOID node1 = *(PVOID *)elem1;
    PVOID node2 = *(PVOID *)elem2;
    LONG result = 0;
    ULONG i;

    for (i = 0; i < sortContext->NumberOfKeys; i++)
    {
        PPH_CM_SORT_KEY key = &sortContext->Keys[i];

        if (key->CompareFunction)
            result = key->CompareFunction(node1, node2, key->SortOrder, key->CompareContext);
        else
            result = key->PluginSortFunction(node1, node2, key->SubId, key->SortOrder, key->PluginContext);

        if (result != 0)
            return PhModifySort(result, key->SortOrder);
    }

    // All keys are equal; use the list's stable tie-breaker. The post-sort
    // function applies the order itself, so pass the primary key's order.
    if (sortContext->PostSortFunction)
        return sortContext->PostSortFunction(0, node1, node2, sortContext->PrimarySortOrder);

    return 0;
}

/**
 * Sorts an array of TreeNew nodes using all of the control's current sort keys
 * (primary key first, then the Shift+click secondary keys).
 *
 * Built-in columns use the functions registered with PhCmRegisterSortFunctions;
 * plugin columns use their registered plugin sort function.
 *
 * \param Manager A pointer to the column manager.
 * \param Nodes An array of pointers to TreeNew nodes to sort.
 * \param NumberOfNodes The number of nodes in the array.
 * \return TRUE if the nodes were sorted, FALSE if there was nothing to sort by.
 */
BOOLEAN PhCmSortNodes(
    _In_ PPH_CM_MANAGER Manager,
    _Inout_updates_(NumberOfNodes) PPH_TREENEW_NODE *Nodes,
    _In_ ULONG NumberOfNodes
    )
{
    return PhCmSortNodesEx(Manager, Nodes, NumberOfNodes, NULL);
}

/**
 * Sorts an array of TreeNew nodes by a set of sort keys.
 *
 * \param Manager A pointer to the column manager.
 * \param Nodes An array of pointers to TreeNew nodes to sort.
 * \param NumberOfNodes The number of nodes in the array.
 * \param SortKeys The sort keys to use, or NULL to use the control's current sort keys.
 * \return TRUE if the nodes were sorted, FALSE if there was nothing to sort by.
 */
BOOLEAN PhCmSortNodesEx(
    _In_ PPH_CM_MANAGER Manager,
    _Inout_updates_(NumberOfNodes) PPH_TREENEW_NODE *Nodes,
    _In_ ULONG NumberOfNodes,
    _In_opt_ PPH_TREENEW_SORT_KEYS SortKeys
    )
{
    PH_TREENEW_SORT_KEYS sortKeys;
    PH_CM_MULTI_SORT_CONTEXT sortContext;
    ULONG i;

    if (!Manager->Handle || NumberOfNodes < 2)
        return FALSE;

    if (SortKeys)
    {
        if (SortKeys->Count > PH_TREENEW_MAX_SORT_KEYS)
            return FALSE;

        sortKeys = *SortKeys;
    }
    else
    {
        if (!TreeNew_GetSortKeys(Manager->Handle, &sortKeys))
            return FALSE;
    }

    if (sortKeys.Count == 0)
        return FALSE;

    memset(&sortContext, 0, sizeof(PH_CM_MULTI_SORT_CONTEXT));
    sortContext.PostSortFunction = Manager->PostSortFunction;

    for (i = 0; i < sortKeys.Count; i++)
    {
        ULONG columnId = sortKeys.Keys[i].Column;
        PPH_CM_SORT_KEY key = &sortContext.Keys[sortContext.NumberOfKeys];

        if (sortKeys.Keys[i].Order == NoSortOrder)
            continue;

        if (columnId < Manager->MinId)
        {
            if (columnId >= Manager->NumberOfSortFunctions || !Manager->SortFunctions[columnId])
                continue;

            key->CompareFunction = Manager->SortFunctions[columnId];
            key->CompareContext = Manager->SortContext;
        }
        else
        {
            PH_TREENEW_COLUMN tnColumn;
            PPH_CM_COLUMN column;

            if (!TreeNew_GetColumn(Manager->Handle, columnId, &tnColumn))
                continue;
            if (!(column = tnColumn.Context) || !column->SortFunction)
                continue;

            key->PluginSortFunction = (PPH_PLUGIN_TREENEW_SORT_FUNCTION)column->SortFunction;
            key->SubId = column->SubId;
            key->PluginContext = column->Context;
        }

        key->SortOrder = sortKeys.Keys[i].Order;

        if (sortContext.NumberOfKeys == 0)
            sortContext.PrimarySortOrder = key->SortOrder;

        sortContext.NumberOfKeys++;
    }

    if (sortContext.NumberOfKeys == 0)
        return FALSE;

    qsort_s(Nodes, NumberOfNodes, sizeof(PVOID), PhCmpMultiSortFunction, &sortContext);

    return TRUE;
}

/**
 * Loads and applies column settings to a TreeNew control.
 *
 * \param TreeNewHandle A handle to the TreeNew control window.
 * \param Settings A string reference containing serialized column settings.
 * \return BOOLEAN TRUE if settings were loaded successfully, FALSE otherwise.
 */
BOOLEAN PhCmLoadSettings(
    _In_ HWND TreeNewHandle,
    _In_ PCPH_STRINGREF Settings
    )
{
    return PhCmLoadSettingsEx(TreeNewHandle, NULL, 0, Settings, NULL);
}

/**
 * Loads and applies column and sort settings to a TreeNew control with optional plugin column support.
 *
 * \param TreeNewHandle A handle to the TreeNew control window.
 * \param Manager An optional pointer to the column manager for plugin columns.
 * \param Flags Option flags, such as PH_CM_COLUMN_WIDTHS_ONLY.
 * \param Settings A string reference containing serialized column settings.
 * \param SortSettings An optional string reference containing serialized sort settings.
 * \return BOOLEAN TRUE if settings were loaded successfully, FALSE otherwise.
 */
BOOLEAN PhCmLoadSettingsEx(
    _In_ HWND TreeNewHandle,
    _In_opt_ PPH_CM_MANAGER Manager,
    _In_ ULONG Flags,
    _In_ PCPH_STRINGREF Settings,
    _In_opt_ PCPH_STRINGREF SortSettings
    )
{
    BOOLEAN result = FALSE;
    PH_STRINGREF scalePart;
    PH_STRINGREF columnPart;
    PH_STRINGREF remainingColumnPart;
    PH_STRINGREF valuePart;
    PH_STRINGREF subPart;
    ULONG64 integer;
    ULONG scale;
    ULONG total;
    BOOLEAN hasFixedColumn;
    ULONG count;
    ULONG i;
    PPH_HASHTABLE columnHashtable;
    PH_HASHTABLE_ENUM_CONTEXT enumContext;
    PPH_KEY_VALUE_PAIR pair;
    ULONG orderArray[PH_CM_ORDER_LIMIT];
    ULONG maxOrder;
    LONG dpiValue;

    dpiValue = PhGetWindowDpi(TreeNewHandle);

    if (Settings->Length != 0)
    {
        columnHashtable = PhCreateSimpleHashtable(20);

        remainingColumnPart = *Settings;

        if (remainingColumnPart.Length != 0 && remainingColumnPart.Buffer[0] == L'@')
        {
            PhSkipStringRef(&remainingColumnPart, sizeof(WCHAR));
            PhSplitStringRefAtChar(&remainingColumnPart, L'|', &scalePart, &remainingColumnPart);

            if (scalePart.Length == 0 || !PhStringToUInt64(&scalePart, 10, &integer))
                goto CleanupExit;

            scale = (ULONG)integer;
        }
        else
        {
            scale = USER_DEFAULT_SCREEN_DPI;
        }

        while (remainingColumnPart.Length != 0)
        {
            PPH_TREENEW_COLUMN column;
            ULONG id;
            ULONG displayIndex;
            LONG width;

            PhSplitStringRefAtChar(&remainingColumnPart, L'|', &columnPart, &remainingColumnPart);

            if (columnPart.Length != 0)
            {
                // Id

                PhSplitStringRefAtChar(&columnPart, L',', &valuePart, &columnPart);

                if (valuePart.Length == 0)
                    goto CleanupExit;

                if (valuePart.Buffer[0] == L'+')
                {
                    PH_STRINGREF pluginName;
                    ULONG subId;
                    PPH_CM_COLUMN cmColumn;

                    // This is a plugin-owned column.

                    if (!Manager)
                        continue;
                    if (!PhEmParseCompoundId(&valuePart, &pluginName, &subId))
                        continue;

                    cmColumn = PhCmFindColumn(Manager, &pluginName, subId);

                    if (!cmColumn)
                        continue; // can't find the column, skip this part

                    id = cmColumn->Id;
                }
                else
                {
                    if (!PhStringToUInt64(&valuePart, 10, &integer))
                        goto CleanupExit;

                    id = (ULONG)integer;
                }

                // Display Index

                PhSplitStringRefAtChar(&columnPart, L',', &valuePart, &columnPart);

                if (!(Flags & PH_CM_COLUMN_WIDTHS_ONLY))
                {
                    if (valuePart.Length == 0 || !PhStringToUInt64(&valuePart, 10, &integer))
                        goto CleanupExit;

                    displayIndex = (ULONG)integer;
                }
                else
                {
                    if (valuePart.Length != 0)
                        goto CleanupExit;

                    displayIndex = ULONG_MAX;
                }

                // Width

                if (columnPart.Length == 0 || !PhStringToUInt64(&columnPart, 10, &integer))
                    goto CleanupExit;

                width = (LONG)integer;

                if (scale != dpiValue && scale != 0)
                    width = PhMultiplyDivideSigned(width, dpiValue, scale);

                column = PhAllocateZero(sizeof(PH_TREENEW_COLUMN));
                column->Id = id;
                column->DisplayIndex = displayIndex;
                column->Width = width;
                PhAddItemSimpleHashtable(columnHashtable, UlongToPtr(column->Id), column);
            }
        }

        TreeNew_SetRedraw(TreeNewHandle, FALSE);

        // Set visibility and width.

        i = 0;
        count = 0;
        total = TreeNew_GetColumnCount(TreeNewHandle);
        hasFixedColumn = !!TreeNew_GetFixedColumn(TreeNewHandle);
        memset(orderArray, 0, sizeof(orderArray));
        maxOrder = 0;

        while (count < total)
        {
            PH_TREENEW_COLUMN setColumn;
            PPH_TREENEW_COLUMN *columnPtr;

            if (TreeNew_GetColumn(TreeNewHandle, i, &setColumn))
            {
                columnPtr = (PPH_TREENEW_COLUMN *)PhFindItemSimpleHashtable(columnHashtable, UlongToPtr(i));

                if (!(Flags & PH_CM_COLUMN_WIDTHS_ONLY))
                {
                    if (columnPtr)
                    {
                        setColumn.Visible = TRUE;
                        setColumn.Width = (*columnPtr)->Width;
                        TreeNew_SetColumn(TreeNewHandle, TN_COLUMN_FLAG_VISIBLE | TN_COLUMN_WIDTH, &setColumn);

                        if (!setColumn.Fixed)
                        {
                            // For compatibility reasons, normal columns have their display indices stored
                            // one higher than usual (so they start from 1, not 0). Fix that here.
                            if (hasFixedColumn && (*columnPtr)->DisplayIndex != 0)
                                (*columnPtr)->DisplayIndex--;

                            if ((*columnPtr)->DisplayIndex < PH_CM_ORDER_LIMIT)
                            {
                                orderArray[(*columnPtr)->DisplayIndex] = i;

                                if (maxOrder < (*columnPtr)->DisplayIndex + 1)
                                    maxOrder = (*columnPtr)->DisplayIndex + 1;
                            }
                        }
                    }
                    else if (!setColumn.Fixed) // never hide the fixed column
                    {
                        setColumn.Visible = FALSE;
                        TreeNew_SetColumn(TreeNewHandle, TN_COLUMN_FLAG_VISIBLE, &setColumn);
                    }
                }
                else
                {
                    if (columnPtr)
                    {
                        setColumn.Width = (*columnPtr)->Width;
                        TreeNew_SetColumn(TreeNewHandle, TN_COLUMN_WIDTH, &setColumn);
                    }
                }

                count++;
            }

            i++;
        }

        if (!(Flags & PH_CM_COLUMN_WIDTHS_ONLY))
        {
            // Set the order array.
            TreeNew_SetColumnOrderArray(TreeNewHandle, maxOrder, orderArray);
        }

        TreeNew_SetRedraw(TreeNewHandle, TRUE);

        result = TRUE;

CleanupExit:
        PhBeginEnumHashtable(columnHashtable, &enumContext);

        while (pair = PhNextEnumHashtable(&enumContext))
            PhFree(pair->Value);

        PhDereferenceObject(columnHashtable);
    }

    // Load sort settings. Format: "col,order[|col,order...]" (primary key first).

    if (SortSettings && SortSettings->Length != 0)
    {
        PH_STRINGREF remainingPart = *SortSettings;
        PH_STRINGREF keyPart;
        PH_TREENEW_SORT_KEYS sortKeys;

        memset(&sortKeys, 0, sizeof(PH_TREENEW_SORT_KEYS));

        while (remainingPart.Length != 0 && sortKeys.Count < PH_TREENEW_MAX_SORT_KEYS)
        {
            PhSplitStringRefAtChar(&remainingPart, L'|', &keyPart, &remainingPart);
            PhSplitStringRefAtChar(&keyPart, L',', &valuePart, &subPart);

            if (valuePart.Length != 0 && subPart.Length != 0)
            {
                ULONG sortColumn;
                PH_SORT_ORDER sortOrder;

                sortColumn = ULONG_MAX;

                if (valuePart.Buffer[0] == L'+')
                {
                    PH_STRINGREF pluginName;
                    ULONG subId;
                    PPH_CM_COLUMN cmColumn;

                    if (
                        Manager &&
                        PhEmParseCompoundId(&valuePart, &pluginName, &subId) &&
                        (cmColumn = PhCmFindColumn(Manager, &pluginName, subId))
                        )
                    {
                        sortColumn = cmColumn->Id;
                    }
                }
                else
                {
                    PhStringToUInt64(&valuePart, 10, &integer);
                    sortColumn = (ULONG)integer;
                }

                PhStringToUInt64(&subPart, 10, &integer);
                sortOrder = (PH_SORT_ORDER)integer;

                if (sortColumn != ULONG_MAX && sortOrder <= DescendingSortOrder)
                {
                    sortKeys.Keys[sortKeys.Count].Column = sortColumn;
                    sortKeys.Keys[sortKeys.Count].Order = sortOrder;
                    sortKeys.Count++;
                }
            }
        }

        if (sortKeys.Count == 1)
            TreeNew_SetSort(TreeNewHandle, sortKeys.Keys[0].Column, sortKeys.Keys[0].Order);
        else if (sortKeys.Count > 1)
            TreeNew_SetSortKeys(TreeNewHandle, &sortKeys);
    }

    return result;
}

/**
 * Serializes column settings for a TreeNew control into a string.
 *
 * \param TreeNewHandle A handle to the TreeNew control window.
 * \return PPH_STRING A string containing the serialized column settings.
 */
PPH_STRING PhCmSaveSettings(
    _In_ HWND TreeNewHandle
    )
{
    return PhCmSaveSettingsEx(TreeNewHandle, NULL, 0, NULL);
}

/**
 * Serializes column and sort settings for a TreeNew control with optional plugin column support.
 *
 * \param TreeNewHandle A handle to the TreeNew control window.
 * \param Manager An optional pointer to the column manager for plugin columns.
 * \param Flags Option flags, such as PH_CM_COLUMN_WIDTHS_ONLY.
 * \param SortSettings Optional receives the serialized sort settings string.
 * \return PPH_STRING A string containing the serialized column settings.
 */
PPH_STRING PhCmSaveSettingsEx(
    _In_ HWND TreeNewHandle,
    _In_opt_ PPH_CM_MANAGER Manager,
    _In_ ULONG Flags,
    _Out_opt_ PPH_STRING *SortSettings
    )
{
    PH_STRING_BUILDER stringBuilder;
    ULONG i = 0;
    ULONG count = 0;
    ULONG total;
    ULONG increment;
    PH_TREENEW_COLUMN column;
    LONG dpiValue;

    dpiValue = PhGetWindowDpi(TreeNewHandle);

    total = TreeNew_GetColumnCount(TreeNewHandle);

    if (TreeNew_GetFixedColumn(TreeNewHandle))
        increment = 1; // the first normal column should have a display index that starts with 1, for compatibility
    else
        increment = 0;

    PhInitializeStringBuilder(&stringBuilder, 100);

    {
        PH_FORMAT format[3];
        SIZE_T returnLength;
        WCHAR buffer[PH_INT64_STR_LEN_1];

        // @%lu|
        PhInitFormatC(&format[0], L'@');
        PhInitFormatU(&format[1], dpiValue);
        PhInitFormatC(&format[2], L'|');

        if (PhFormatToBuffer(format, RTL_NUMBER_OF(format), buffer, sizeof(buffer), &returnLength))
        {
            PhAppendStringBuilderEx(&stringBuilder, buffer, returnLength - sizeof(UNICODE_NULL));
        }
        else
        {
            PhAppendFormatStringBuilder(&stringBuilder, L"@%lu|", dpiValue);
        }
    }

    while (count < total)
    {
        if (TreeNew_GetColumn(TreeNewHandle, i, &column))
        {
            if (!(Flags & PH_CM_COLUMN_WIDTHS_ONLY))
            {
                if (column.Visible)
                {
                    if (!Manager || i < Manager->MinId)
                    {
                        PH_FORMAT format[6];
                        SIZE_T returnLength;
                        WCHAR buffer[PH_INT64_STR_LEN_1];

                        // %lu,%lu,%ld|
                        PhInitFormatU(&format[0], i);
                        PhInitFormatC(&format[1], L',');
                        PhInitFormatU(&format[2], column.Fixed ? 0 : column.DisplayIndex + increment);
                        PhInitFormatC(&format[3], L',');
                        PhInitFormatU(&format[4], column.Width);
                        PhInitFormatC(&format[5], L'|');

                        if (PhFormatToBuffer(format, RTL_NUMBER_OF(format), buffer, sizeof(buffer), &returnLength))
                        {
                            PhAppendStringBuilderEx(&stringBuilder, buffer, returnLength - sizeof(UNICODE_NULL));
                        }
                        else
                        {
                            PhAppendFormatStringBuilder(
                                &stringBuilder,
                                L"%lu,%lu,%ld|",
                                i,
                                column.Fixed ? 0 : column.DisplayIndex + increment,
                                column.Width
                                );
                        }
                    }
                    else
                    {
                        PPH_CM_COLUMN cmColumn = column.Context;
                        PH_FORMAT format[9];
                        SIZE_T returnLength;
                        WCHAR buffer[PH_INT64_STR_LEN_1];

                        // +%s+%lu,%lu,%ld|
                        PhInitFormatC(&format[0], L'+');
                        PhInitFormatSR(&format[1], cmColumn->Plugin->Name);
                        PhInitFormatC(&format[2], L'+');
                        PhInitFormatU(&format[3], cmColumn->SubId);
                        PhInitFormatC(&format[4], L',');
                        PhInitFormatU(&format[5], column.DisplayIndex + increment);
                        PhInitFormatC(&format[6], L',');
                        PhInitFormatD(&format[7], column.Width);
                        PhInitFormatC(&format[8], L'|');

                        if (PhFormatToBuffer(format, RTL_NUMBER_OF(format), buffer, sizeof(buffer), &returnLength))
                        {
                            PhAppendStringBuilderEx(&stringBuilder, buffer, returnLength - sizeof(UNICODE_NULL));
                        }
                        else
                        {
                            PhAppendFormatStringBuilder(
                                &stringBuilder,
                                L"+%s+%lu,%lu,%ld|",
                                cmColumn->Plugin->Name.Buffer,
                                cmColumn->SubId,
                                column.DisplayIndex + increment,
                                column.Width
                                );
                        }
                    }
                }
            }
            else
            {
                if (!Manager || i < Manager->MinId)
                {
                    PhAppendFormatStringBuilder(
                        &stringBuilder,
                        L"%lu,,%ld|",
                        i,
                        column.Width
                        );
                }
                else
                {
                    PPH_CM_COLUMN cmColumn;

                    cmColumn = column.Context;
                    PhAppendFormatStringBuilder(
                        &stringBuilder,
                        L"+%s+%lu,,%ld|",
                        cmColumn->Plugin->Name.Buffer,
                        cmColumn->SubId,
                        column.Width
                        );
                }
            }

            count++;
        }

        i++;
    }

    if (stringBuilder.String->Length != 0)
        PhRemoveEndStringBuilder(&stringBuilder, 1);

    if (SortSettings)
    {
        PH_TREENEW_SORT_KEYS sortKeys;

        if (TreeNew_GetSortKeys(TreeNewHandle, &sortKeys))
        {
            if (sortKeys.Count != 0)
            {
                PH_STRING_BUILDER sortBuilder;

                PhInitializeStringBuilder(&sortBuilder, 32);

                for (ULONG k = 0; k < sortKeys.Count; k++)
                {
                    ULONG sortColumn = sortKeys.Keys[k].Column;
                    PH_SORT_ORDER sortOrder = sortKeys.Keys[k].Order;

                    if (!Manager || sortColumn < Manager->MinId)
                    {
                        PhAppendFormatStringBuilder(&sortBuilder, L"%lu,%lu|", sortColumn, (ULONG)sortOrder);
                    }
                    else if (TreeNew_GetColumn(TreeNewHandle, sortColumn, &column))
                    {
                        PPH_CM_COLUMN cmColumn = column.Context;

                        PhAppendCharStringBuilder(&sortBuilder, L'+');
                        PhAppendStringBuilder(&sortBuilder, &cmColumn->Plugin->Name);
                        PhAppendFormatStringBuilder(&sortBuilder, L"+%lu,%lu|", cmColumn->SubId, (ULONG)sortOrder);
                    }
                }

                if (sortBuilder.String->Length != 0)
                    PhRemoveEndStringBuilder(&sortBuilder, 1);

                *SortSettings = PhFinalStringBuilderString(&sortBuilder);
            }
            else
            {
                *SortSettings = PhCreateString(L"0,0");
            }
        }
        else
        {
            *SortSettings = PhReferenceEmptyString();
        }
    }

    return PhFinalStringBuilderString(&stringBuilder);
}
